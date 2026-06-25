#include "app/app_state.hpp"
#include "app/app_state_internal.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <thread>

#include <nlohmann/json.hpp>

#include "app/app_paths.hpp"
#include "core/account_store/ban_diff.hpp"
#include "core/account_store/store.hpp"
#include "core/crypto/rng.hpp"
#include "core/cs2_gc/cs2_gc_client.hpp"
#include "core/http/client.hpp"
#include "core/log.hpp"
#include "core/steam_api/ban_check.hpp"
#include "core/update_check.hpp"
#include "core/version.hpp"
#include "platform/startup_task.hpp"
#include "platform/tray_icon.hpp"
#include "ui/screens/cs2_screen_state.hpp"

namespace sam::app {

namespace detail {
// password/tokens are SecureString and self-wipe; the SteamGuard secrets and
// session id are plain std::string, so wipe them before their storage is freed.
void scrub_account_secrets(core::Account& a) {
    const auto wipe = [](std::string& s) {
        if (!s.empty()) crypto::zero_buffer(&s[0], s.size());
    };
    wipe(a.session_id);
    if (a.sda) {
        wipe(a.sda->shared_secret);
        wipe(a.sda->identity_secret);
        wipe(a.sda->revocation_code);
        wipe(a.sda->secret_1);
        wipe(a.sda->uri);
        wipe(a.sda->serial_number);
        wipe(a.sda->token_gid);
        wipe(a.sda->account_name);
        wipe(a.sda->device_id);
    }
}

bool event_enabled(const Settings::NotificationToggles& t, core::BanEventKind k) {
    using K = core::BanEventKind;
    switch (k) {
        case K::NewVacBan:       return t.on_new_vac_ban;
        case K::NewGameBan:      return t.on_new_game_ban;
        case K::NewCommunityBan: return t.on_new_community_ban;
        case K::NewTradeBan:     return t.on_new_trade_ban;
        case K::NewVacLive:      return t.on_new_vac_live;
        case K::BanRemoved:      return t.on_ban_removed;
        case K::CooldownStarted: return t.on_cooldown_started;
        case K::CooldownEnded:   return t.on_cooldown_ended;
    }
    return false;
}

std::string toast_message_for(const core::Account& a, const core::BanEvent& ev) {
    const char* label = ban_event_kind_label(ev.kind);
    const std::string who = !a.web.persona_name.empty()
        ? a.web.persona_name : a.login;
    return std::string(label) + " - " + who;
}

void push_toasts_for(AppState& state, const core::Account& a,
                      const std::vector<core::BanEvent>& events,
                      std::int64_t now) {
    if (!state.settings.notifications.surface_toast) return;
    for (const auto& ev : events) {
        ui::widgets::ToastItem t;
        t.id = ev.event_id;
        t.message = toast_message_for(a, ev);
        t.account_id = a.id;
        t.is_warning = (ev.kind == core::BanEventKind::CooldownStarted ||
                        ev.kind == core::BanEventKind::CooldownEnded);
        t.expires_at_unix = now + state.settings.notifications.toast_duration_seconds;
        state.toasts.push(std::move(t));
    }
}

bool ban_event_is_cooldown(core::BanEventKind k) {
    return k == core::BanEventKind::CooldownStarted ||
           k == core::BanEventKind::CooldownEnded;
}

// One-shot balloon for a manual refresh. Suppressed while the main window is
// focused (the in-app toast covers that).
void push_native_notification(AppState& state, const std::string& message, bool warning) {
    if (!state.settings.notifications.enabled ||
        !state.settings.notifications.surface_windows_notification) {
        return;
    }
    if (platform::tray_icon::owner_is_foreground()) return;
    if (platform::tray_icon::show_balloon("Steam Account Manager", message, warning)) {
        state.balloon_shown.store(true, std::memory_order_relaxed);
    }
}

// Records batch events so they coalesce into one balloon.
void note_session_event(AppState& state, const core::Account& a,
                        const std::vector<core::BanEvent>& events) {
    if (events.empty()) return;
    if (state.session_event_count == 0) {
        state.session_event_message = toast_message_for(a, events.front());
        state.session_event_warning = ban_event_is_cooldown(events.front().kind);
    }
    state.session_event_count += static_cast<int>(events.size());
}

// Shows the accumulated batch events as one balloon, then clears the accumulator.
void flush_native_notification(AppState& state) {
    if (!state.settings.notifications.enabled ||
        !state.settings.notifications.surface_windows_notification) {
        return;
    }
    const int n = state.session_event_count;
    if (n == 0) return;
    if (platform::tray_icon::owner_is_foreground()) return;
    const std::string msg = (n == 1)
        ? state.session_event_message
        : std::to_string(n) + " new ban / cooldown changes";
    if (platform::tray_icon::show_balloon("Steam Account Manager", msg,
                                          n == 1 && state.session_event_warning)) {
        state.balloon_shown.store(true, std::memory_order_relaxed);
    }
    state.session_event_count = 0;
    state.session_event_message.clear();
}
}  // namespace detail

AppState::AppState() = default;
AppState::~AppState() = default;

core::Account* AppState::find_account(const std::string& id) {
    for (auto& a : vault.accounts) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

void AppState::apply_gc_snapshot_cache(const std::string& account_id,
                                       const cs2_gc::Snapshot& snap) {
    core::Account* acc = find_account(account_id);
    if (acc == nullptr) return;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    acc->cs2.gc_last_pulled_unix = now;
    if (snap.progress.valid) {
        acc->cs2.cs2_player_level = snap.progress.level;
        acc->cs2.cs2_player_xp = snap.progress.xp_in_level;
    }
    acc->cs2.featured_medal_defidx = snap.featured_medal_defidx;
    acc->cs2.medals.clear();
    acc->cs2.medals.reserve(snap.medals.size());
    for (const auto& m : snap.medals)
        acc->cs2.medals.push_back({static_cast<std::uint32_t>(m.id), m.name, m.icon_url});
    vault_dirty = true;
    save_vault_if_dirty();
}

void AppState::save_vault_if_dirty() {
    if (!vault_dirty || master_password.empty()) return;
    vault_saver.start(vault_path());
    vault_saver.schedule(vault, master_password);
    vault_dirty = false;
}

void AppState::flush_pending_save() {
    // Schedule a still-dirty vault so it gets flushed.
    if (vault_dirty && !master_password.empty()) {
        vault_saver.start(vault_path());
        vault_saver.schedule(vault, master_password);
        vault_dirty = false;
    }
    vault_saver.flush();
}

void AppState::post_ui_callback(std::function<void()> fn) {
    std::lock_guard lk(job_mutex);
    completed_jobs.push_back({"", std::move(fn)});
}

void AppState::start_update_check() {
    if (!settings.check_updates_on_launch) {
        return;
    }
    update_thread = std::jthread([this](std::stop_token st) {
        if (st.stop_requested()) {
            return;
        }
        auto r = core::update_check::fetch_latest_release(
            "luminary-cloud/steam-account-manager", sam::kVersion);
        if (!r || !r->newer_than_current || st.stop_requested()) {
            return;
        }
        std::lock_guard lk(update_mutex);
        update_result = std::move(r);
    });
}

void AppState::sync_proxy_policy() {
    http::ProxyMode mode = http::ProxyMode::Direct;
    switch (settings.proxy_mode) {
        case ProxyMode::None:       mode = http::ProxyMode::Direct;     break;
        case ProxyMode::Single:     mode = http::ProxyMode::Single;     break;
        case ProxyMode::PerAccount: mode = http::ProxyMode::PerAccount; break;
    }
    http::set_proxy_policy(mode, settings.single_proxy);
}

void AppState::save_settings() {
    nlohmann::json j;
    j["clipboard_clear_seconds"] = settings.clipboard_clear_seconds;
    j["auto_lock_minutes"]       = settings.auto_lock_minutes;
    j["accounts_view"]           = static_cast<int>(settings.accounts_view);
    j["show_avatars"]            = settings.show_avatars;
    j["hide_notes"]              = settings.hide_notes;
    j["refresh_on_launch"]       = settings.refresh_on_launch;
    j["gcpd_enabled"]            = settings.gcpd_enabled;
    j["remember_master_password"] = settings.remember_master_password;
    j["logon_action"]            = static_cast<int>(settings.logon_action);
    j["start_minimized"]         = settings.start_minimized;
    // Downgrade-safe: older builds key off this bool for the headless logon refresh.
    j["start_with_windows"]      = (settings.logon_action == LogonAction::BackgroundRefresh);
    j["privacy_mode"]            = settings.privacy_mode;
    j["streamproof"]             = settings.streamproof;
    j["disable_cloud_on_login"]  = settings.disable_cloud_on_login;
    j["disable_news_on_login"]   = settings.disable_news_on_login;
    j["remember_password_on_login"] = settings.remember_password_on_login;
    j["web_api_key"]             = settings.web_api_key;
    j["proxy_mode"]              = static_cast<int>(settings.proxy_mode);
    j["single_proxy"]            = settings.single_proxy;
    j["check_updates_on_launch"]  = settings.check_updates_on_launch;
    j["version_check_skip_until"] = settings.version_check_skip_until;

    auto& info = j["info"];
    info["show_vac"]           = settings.info.show_vac;
    info["show_game_ban"]      = settings.info.show_game_ban;
    info["show_community_ban"] = settings.info.show_community_ban;
    info["show_trade_ban"]     = settings.info.show_trade_ban;
    info["show_steam_level"]   = settings.info.show_steam_level;
    info["show_owned_games"]   = settings.info.show_owned_games;
    info["show_premier"]       = settings.info.show_premier;
    info["show_wingman"]       = settings.info.show_wingman;
    info["show_prime"]         = settings.info.show_prime;
    info["show_vac_live"]      = settings.info.show_vac_live;
    info["show_cooldown"]      = settings.info.show_cooldown;
    info["show_weekly_drop"]   = settings.info.show_weekly_drop;
    info["show_external_funds"] = settings.info.show_external_funds;

    auto& notif = j["notifications"];
    notif["enabled"]               = settings.notifications.enabled;
    notif["surface_in_card"]       = settings.notifications.surface_in_card;
    notif["surface_toast"]         = settings.notifications.surface_toast;
    notif["surface_windows_notification"] = settings.notifications.surface_windows_notification;
    notif["on_new_vac_ban"]        = settings.notifications.on_new_vac_ban;
    notif["on_new_game_ban"]       = settings.notifications.on_new_game_ban;
    notif["on_new_community_ban"]  = settings.notifications.on_new_community_ban;
    notif["on_new_trade_ban"]      = settings.notifications.on_new_trade_ban;
    notif["on_new_vac_live"]       = settings.notifications.on_new_vac_live;
    notif["on_ban_removed"]        = settings.notifications.on_ban_removed;
    notif["on_cooldown_started"]   = settings.notifications.on_cooldown_started;
    notif["on_cooldown_ended"]     = settings.notifications.on_cooldown_ended;
    notif["toast_duration_seconds"] = settings.notifications.toast_duration_seconds;
    notif["coalesce_threshold"]    = settings.notifications.coalesce_threshold;
    notif["retention_days"]        = settings.notifications.retention_days;

    auto& lv = j["list_view"];
    lv["show_cooldown_marker"]    = settings.list_view.show_cooldown_marker;
    lv["show_unread_badge"]       = settings.list_view.show_unread_badge;
    lv["show_weekly_drop_marker"] = settings.list_view.show_weekly_drop_marker;
    lv["hide_account_name"]       = settings.list_view.hide_account_name;

    auto& sj = j["sda"];
    sj["auto_copy_on_select"]    = settings.sda.auto_copy_on_select;
    sj["show_next_code"]         = settings.sda.show_next_code;
    sj["hide_current_code"]      = settings.sda.hide_current_code;
    sj["global_hotkey_enabled"]  = settings.sda.global_hotkey_enabled;
    sj["global_hotkey_mods"]     = settings.sda.global_hotkey_mods;
    sj["global_hotkey_vk"]       = settings.sda.global_hotkey_vk;

    auto& cj = j["confirmations"];
    cj["per_account_cooldown_seconds"] = settings.confirmations.per_account_cooldown_seconds;
    cj["refresh_stagger_ms"]           = settings.confirmations.refresh_stagger_ms;
    cj["bulk_size_cap"]                = settings.confirmations.bulk_size_cap;
    cj["permanent_failure_threshold"]  = settings.confirmations.permanent_failure_threshold;
    cj["background_poll_enabled"]      = settings.confirmations.background_poll_enabled;
    cj["background_poll_minutes"]      = settings.confirmations.background_poll_minutes;
    cj["toast_on_new_confirmations"]   = settings.confirmations.toast_on_new_confirmations;
    cj["show_account_search"]          = settings.confirmations.show_account_search;
    cj["auto_approve_enabled"]         = settings.confirmations.auto_approve_enabled;
    cj["auto_approve_market"]          = settings.confirmations.auto_approve_market;
    cj["auto_approve_phone_change"]    = settings.confirmations.auto_approve_phone_change;
    cj["auto_approve_trade_partners"]  = settings.confirmations.auto_approve_trade_partners;
    cj["audit_retention_days"]         = settings.confirmations.audit_retention_days;

    auto& tj = j["trade"];
    tj["default_destination_trade_url"] = settings.trade.default_destination_trade_url;
    auto& sl = tj["saved_trade_urls"] = nlohmann::json::array();
    for (const auto& l : settings.trade.saved_trade_urls) {
        nlohmann::json o;
        o["url"]  = l.url;
        o["name"] = l.name;
        sl.push_back(std::move(o));
    }
    tj["auto_confirm_sent"]             = settings.trade.auto_confirm_sent;
    tj["per_account_cooldown_seconds"]  = settings.trade.per_account_cooldown_seconds;
    tj["inventory_cooldown_seconds"]    = settings.trade.inventory_cooldown_seconds;
    tj["refresh_stagger_ms"]            = settings.trade.refresh_stagger_ms;
    tj["background_poll_enabled"]       = settings.trade.background_poll_enabled;
    tj["background_poll_seconds"]       = settings.trade.background_poll_seconds;

    j["accounts_sort"] = settings.accounts_sort;
    j["collapsed_groups"] = settings.collapsed_groups;
    auto& qf = j["quick_filters"];
    qf["only_banned"]   = settings.quick_filters.only_banned;
    qf["only_cooldown"] = settings.quick_filters.only_cooldown;
    qf["only_prime"]    = settings.quick_filters.only_prime;

    auto& vj = j["cs2_video"];
    vj["mode"]                = static_cast<int>(settings.cs2_video.mode);
    vj["source_label"]        = settings.cs2_video.source_label;
    vj["folder_source_label"] = settings.cs2_video.folder_source_label;
    vj["launch_options"]      = settings.cs2_video.launch_options;
    // Downgrade-safe: older builds key off this bool for video.txt mode.
    vj["auto_apply_on_login"] = (settings.cs2_video.mode == CS2ConfigMode::VideoTxt);

    auto& gj = j["cs2_gc"];
    gj["enabled"]             = settings.cs2_gc.enabled;
    gj["show_weekly_drop"]    = settings.cs2_gc.show_weekly_drop;
    gj["show_weekly_mission"] = settings.cs2_gc.show_weekly_mission;
    gj["show_inventory"]      = settings.cs2_gc.show_inventory;
    gj["show_storage_units"]  = settings.cs2_gc.show_storage_units;
    gj["auto_mark_claimed"]   = settings.cs2_gc.auto_mark_claimed;
    gj["auto_pull_on_startup"] = settings.cs2_gc.auto_pull_on_startup;
    gj["cache_hours"]         = settings.cs2_gc.cache_hours;

    auto path = settings_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (out) {
        out << j.dump(4);
    }
}

void AppState::load_settings() {
    // A reload is lock-equivalent: privacy_mode reveals must not survive it.
    clear_session_secrets();

    auto path = settings_path();
    std::ifstream in(path);
    if (!in) return;

    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }

    auto get = [&](const char* key, auto& dst) {
        if (j.contains(key)) {
            dst = j[key].get<std::remove_reference_t<decltype(dst)>>();
        }
    };

    get("clipboard_clear_seconds", settings.clipboard_clear_seconds);
    settings.clipboard_clear_seconds =
        std::clamp(settings.clipboard_clear_seconds, 10, 120);
    get("auto_lock_minutes",       settings.auto_lock_minutes);
    if (j.contains("accounts_view")) {
        const int v = j["accounts_view"].get<int>();
        settings.accounts_view = (v == 1) ? AccountsViewMode::List : AccountsViewMode::Grid;
    }
    get("show_avatars",            settings.show_avatars);
    get("hide_notes",              settings.hide_notes);
    get("collapsed_groups",        settings.collapsed_groups);
    get("refresh_on_launch",       settings.refresh_on_launch);
    get("gcpd_enabled",            settings.gcpd_enabled);
    get("remember_master_password", settings.remember_master_password);
    if (j.contains("logon_action")) {
        int v = j["logon_action"].get<int>();
        if (v < 0 || v > 2) v = 0;
        settings.logon_action = static_cast<LogonAction>(v);
    } else if (j.contains("start_with_windows") && j["start_with_windows"].get<bool>()) {
        // Migrate the legacy on/off bool (only ever meant the headless refresh).
        settings.logon_action = LogonAction::BackgroundRefresh;
    }
    get("start_minimized",         settings.start_minimized);
    get("privacy_mode",            settings.privacy_mode);
    get("streamproof",             settings.streamproof);
    get("disable_cloud_on_login",  settings.disable_cloud_on_login);
    get("disable_news_on_login",   settings.disable_news_on_login);
    get("remember_password_on_login", settings.remember_password_on_login);
    get("web_api_key",             settings.web_api_key);
    get("single_proxy",            settings.single_proxy);
    if (j.contains("proxy_mode")) {
        settings.proxy_mode = static_cast<ProxyMode>(j["proxy_mode"].get<int>());
    }
    sync_proxy_policy();
    get("check_updates_on_launch",  settings.check_updates_on_launch);
    get("version_check_skip_until", settings.version_check_skip_until);

    if (j.contains("info")) {
        auto& ij = j["info"];
        auto get_info = [&](const char* key, auto& dst) {
            if (ij.contains(key)) {
                dst = ij[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_info("show_vac",           settings.info.show_vac);
        get_info("show_game_ban",      settings.info.show_game_ban);
        get_info("show_community_ban", settings.info.show_community_ban);
        get_info("show_trade_ban",     settings.info.show_trade_ban);
        get_info("show_steam_level",   settings.info.show_steam_level);
        get_info("show_owned_games",   settings.info.show_owned_games);
        get_info("show_premier",       settings.info.show_premier);
        get_info("show_wingman",       settings.info.show_wingman);
        get_info("show_prime",         settings.info.show_prime);
        get_info("show_vac_live",      settings.info.show_vac_live);
        get_info("show_cooldown",      settings.info.show_cooldown);
        get_info("show_weekly_drop",   settings.info.show_weekly_drop);
        get_info("show_external_funds", settings.info.show_external_funds);
    }

    if (j.contains("notifications")) {
        auto& nj = j["notifications"];
        auto get_n = [&](const char* key, auto& dst) {
            if (nj.contains(key)) {
                dst = nj[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_n("enabled",               settings.notifications.enabled);
        get_n("surface_in_card",       settings.notifications.surface_in_card);
        get_n("surface_toast",         settings.notifications.surface_toast);
        get_n("surface_windows_notification", settings.notifications.surface_windows_notification);
        get_n("on_new_vac_ban",        settings.notifications.on_new_vac_ban);
        get_n("on_new_game_ban",       settings.notifications.on_new_game_ban);
        get_n("on_new_community_ban",  settings.notifications.on_new_community_ban);
        get_n("on_new_trade_ban",      settings.notifications.on_new_trade_ban);
        get_n("on_new_vac_live",       settings.notifications.on_new_vac_live);
        get_n("on_ban_removed",        settings.notifications.on_ban_removed);
        get_n("on_cooldown_started",   settings.notifications.on_cooldown_started);
        get_n("on_cooldown_ended",     settings.notifications.on_cooldown_ended);
        get_n("toast_duration_seconds", settings.notifications.toast_duration_seconds);
        get_n("coalesce_threshold",    settings.notifications.coalesce_threshold);
        get_n("retention_days",        settings.notifications.retention_days);
    }

    if (j.contains("list_view")) {
        auto& lj = j["list_view"];
        auto get_l = [&](const char* key, auto& dst) {
            if (lj.contains(key)) {
                dst = lj[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_l("show_cooldown_marker",    settings.list_view.show_cooldown_marker);
        get_l("show_unread_badge",       settings.list_view.show_unread_badge);
        get_l("show_weekly_drop_marker", settings.list_view.show_weekly_drop_marker);
        get_l("hide_account_name",       settings.list_view.hide_account_name);
    }

    if (j.contains("sda")) {
        auto& sj = j["sda"];
        auto get_s = [&](const char* key, auto& dst) {
            if (sj.contains(key)) {
                dst = sj[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_s("auto_copy_on_select",   settings.sda.auto_copy_on_select);
        get_s("show_next_code",        settings.sda.show_next_code);
        get_s("hide_current_code",     settings.sda.hide_current_code);
        get_s("global_hotkey_enabled", settings.sda.global_hotkey_enabled);
        get_s("global_hotkey_mods",    settings.sda.global_hotkey_mods);
        get_s("global_hotkey_vk",      settings.sda.global_hotkey_vk);
    }

    if (j.contains("confirmations")) {
        auto& cj = j["confirmations"];
        auto get_c = [&](const char* key, auto& dst) {
            if (cj.contains(key)) {
                dst = cj[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_c("per_account_cooldown_seconds", settings.confirmations.per_account_cooldown_seconds);
        get_c("refresh_stagger_ms",           settings.confirmations.refresh_stagger_ms);
        get_c("bulk_size_cap",                settings.confirmations.bulk_size_cap);
        get_c("permanent_failure_threshold",  settings.confirmations.permanent_failure_threshold);
        get_c("background_poll_enabled",      settings.confirmations.background_poll_enabled);
        get_c("background_poll_minutes",      settings.confirmations.background_poll_minutes);
        get_c("toast_on_new_confirmations",   settings.confirmations.toast_on_new_confirmations);
        get_c("show_account_search",          settings.confirmations.show_account_search);
        get_c("auto_approve_enabled",         settings.confirmations.auto_approve_enabled);
        get_c("auto_approve_market",          settings.confirmations.auto_approve_market);
        get_c("auto_approve_phone_change",    settings.confirmations.auto_approve_phone_change);
        get_c("auto_approve_trade_partners",  settings.confirmations.auto_approve_trade_partners);
        get_c("audit_retention_days",         settings.confirmations.audit_retention_days);
    }

    if (j.contains("trade")) {
        auto& tj = j["trade"];
        auto get_t = [&](const char* key, auto& dst) {
            if (tj.contains(key)) {
                dst = tj[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_t("default_destination_trade_url", settings.trade.default_destination_trade_url);
        // Accept both the legacy form (array of URL strings) and the current form
        // (array of {url, name} objects); legacy files migrate on the next save.
        if (tj.contains("saved_trade_urls") && tj["saved_trade_urls"].is_array()) {
            settings.trade.saved_trade_urls.clear();
            for (const auto& e : tj["saved_trade_urls"]) {
                if (e.is_string()) {
                    settings.trade.saved_trade_urls.push_back({e.get<std::string>(), {}});
                } else if (e.is_object()) {
                    SavedTradeLink l;
                    l.url  = e.value("url", std::string{});
                    l.name = e.value("name", std::string{});
                    if (!l.url.empty()) settings.trade.saved_trade_urls.push_back(std::move(l));
                }
            }
        }
        get_t("auto_confirm_sent",             settings.trade.auto_confirm_sent);
        get_t("per_account_cooldown_seconds",  settings.trade.per_account_cooldown_seconds);
        get_t("inventory_cooldown_seconds",    settings.trade.inventory_cooldown_seconds);
        get_t("refresh_stagger_ms",            settings.trade.refresh_stagger_ms);
        get_t("background_poll_enabled",       settings.trade.background_poll_enabled);
        get_t("background_poll_seconds",       settings.trade.background_poll_seconds);
    }

    get("accounts_sort", settings.accounts_sort);
    if (j.contains("quick_filters")) {
        auto& qj = j["quick_filters"];
        auto get_q = [&](const char* key, auto& dst) {
            if (qj.contains(key)) {
                dst = qj[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_q("only_banned",   settings.quick_filters.only_banned);
        get_q("only_cooldown", settings.quick_filters.only_cooldown);
        get_q("only_prime",    settings.quick_filters.only_prime);
    }

    if (j.contains("cs2_video")) {
        auto& vj = j["cs2_video"];
        auto get_v = [&](const char* key, auto& dst) {
            if (vj.contains(key)) {
                dst = vj[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_v("source_label",        settings.cs2_video.source_label);
        get_v("folder_source_label", settings.cs2_video.folder_source_label);
        get_v("launch_options",      settings.cs2_video.launch_options);

        if (vj.contains("mode")) {
            int m = vj["mode"].get<int>();
            if (m < 0 || m > 2) m = 0;
            settings.cs2_video.mode = static_cast<CS2ConfigMode>(m);
        } else {
            // Migrate from the legacy on/off bool.
            const bool legacy = vj.contains("auto_apply_on_login") &&
                                vj["auto_apply_on_login"].get<bool>();
            settings.cs2_video.mode =
                legacy ? CS2ConfigMode::VideoTxt : CS2ConfigMode::None;
        }
    }

    if (j.contains("cs2_gc")) {
        auto& gj = j["cs2_gc"];
        auto get_g = [&](const char* key, auto& dst) {
            if (gj.contains(key)) {
                dst = gj[key].get<std::remove_reference_t<decltype(dst)>>();
            }
        };
        get_g("enabled",             settings.cs2_gc.enabled);
        get_g("show_weekly_drop",    settings.cs2_gc.show_weekly_drop);
        get_g("show_weekly_mission", settings.cs2_gc.show_weekly_mission);
        get_g("show_inventory",      settings.cs2_gc.show_inventory);
        get_g("show_storage_units",  settings.cs2_gc.show_storage_units);
        get_g("auto_mark_claimed",   settings.cs2_gc.auto_mark_claimed);
        get_g("auto_pull_on_startup", settings.cs2_gc.auto_pull_on_startup);
        get_g("cache_hours",         settings.cs2_gc.cache_hours);
        settings.cs2_gc.cache_hours = std::clamp(settings.cs2_gc.cache_hours, 1, 24);
    }
}

void AppState::sync_logon_task() const {
    switch (settings.logon_action) {
        case LogonAction::BackgroundRefresh:
            platform::startup_task::set_run_at_logon(true, L"--startup", false);
            break;
        case LogonAction::OpenApp:
            platform::startup_task::set_run_at_logon(
                true, settings.start_minimized ? L"--minimized" : L"", true);
            break;
        case LogonAction::None:
        default:
            if (platform::startup_task::is_run_at_logon_enabled()) {
                platform::startup_task::set_run_at_logon(false);
            }
            break;
    }
}

void AppState::clear_session_secrets() {
    cs2_screen.reset();  // stops the CS2 GC worker and drops its session
    revealed_logins.clear();
    selection_mode = false;
    selected_account_ids.clear();
}

void AppState::lock_vault() {
    clear_session_secrets();
    master_password = crypto::SecureString();
    scrub_vault_secrets();
    vault = core::Vault();
    vault_dirty = false;
    unlocked = false;
    current_screen = Screen::Unlock;
}

void AppState::scrub_vault_secrets() {
    for (auto& a : vault.accounts) detail::scrub_account_secrets(a);
}

void AppState::remove_accounts(std::unordered_set<std::string> ids) {
    if (ids.empty()) return;
    auto& accs = vault.accounts;
    for (auto& a : accs) {
        if (ids.count(a.id) != 0) detail::scrub_account_secrets(a);
    }
    accs.erase(std::remove_if(accs.begin(), accs.end(),
                   [&](const core::Account& x) { return ids.count(x.id) != 0; }),
               accs.end());
    for (const auto& id : ids) {
        last_refresh_unix.erase(id);
        last_gcpd_refresh_unix.erase(id);
        last_persona_change_unix.erase(id);
        conf_last_refresh_unix.erase(id);
        conf_consecutive_failures.erase(id);
        conf_permanent_failure.erase(id);
        refreshing_ids.erase(id);
        revealed_logins.erase(id);
        selected_account_ids.erase(id);
    }
    std::lock_guard<std::mutex> lk(relogin_mutex);
    for (const auto& id : ids) last_relogin_attempt.erase(id);
}

void AppState::enter_selection_mode() {
    selection_mode = true;
    selected_account_ids.clear();
}

void AppState::exit_selection_mode() {
    selection_mode = false;
    selected_account_ids.clear();
}

void AppState::toggle_selected(const std::string& id) {
    auto it = selected_account_ids.find(id);
    if (it == selected_account_ids.end()) {
        selected_account_ids.insert(id);
    } else {
        selected_account_ids.erase(it);
    }
}

bool AppState::is_selected(const std::string& id) const {
    return selected_account_ids.count(id) != 0;
}

}  // namespace sam::app
