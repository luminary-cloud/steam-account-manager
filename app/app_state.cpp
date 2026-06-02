#include "app/app_state.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <thread>

#include <nlohmann/json.hpp>

#include "app/app_paths.hpp"
#include "app/job_pump.hpp"
#include "core/account_store/ban_diff.hpp"
#include "core/cs2_config/video_config.hpp"
#include "core/account_store/store.hpp"
#include "core/crypto/rng.hpp"
#include "core/log.hpp"
#include "core/steam_api/ban_check.hpp"
#include "core/steam_api/summaries.hpp"
#include "core/steam_gcpd/gcpd_parser.hpp"
#include "core/steam_gcpd/gcpd_scraper.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/steam_login/mobile_auth.hpp"
#include "core/steam_login/session.hpp"
#include "core/update_check.hpp"
#include "core/version.hpp"
#include "platform/tray_icon.hpp"

namespace sam::app {

namespace {
// Save coalescing: collapses bursts of vault_dirty events into a single write.
constexpr std::chrono::milliseconds kSaveDebounce{250};

// Per-account refresh cooldowns. The GCPD scrape is rate-limited
// independently so the cheap Web API portion can still run more often.
constexpr std::int64_t kMinAccountRefreshSeconds = 30;
constexpr std::int64_t kMinBatchRefreshSeconds = 120;
constexpr std::int64_t kMinGcpdRefreshSeconds = 90;

// Per-account cooldown between display-name changes. Steam can flag accounts
// for rapid persona renames, so a successful change locks the action out for
// this long.
constexpr std::int64_t kPersonaChangeCooldownSeconds = 300;

// Overwrite the secret bytes a removed or locked account leaves in the heap.
// The password and token fields are crypto::SecureString and wipe themselves;
// the SteamGuard secrets and session id are plain std::string, so wipe them
// here before their storage is freed.
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

// Diffs the about-to-be-written ban/cs2 state, persists every enabled event,
// and updates a.prev_snapshot. Returns the persisted events so the caller
// can decide how to surface them (individual toasts vs a summary).
std::vector<core::BanEvent>
apply_diff_and_snapshot(AppState& state, core::Account& a,
                        const core::BanStatus& new_bans,
                        const core::CS2Status& new_cs2,
                        std::int64_t now) {
    std::vector<core::BanEvent> persisted;
    if (state.settings.notifications.enabled) {
        auto events = core::diff_account_state(a.id, a.prev_snapshot,
                                                new_bans, new_cs2, now);
        for (auto& ev : events) {
            if (!event_enabled(state.settings.notifications, ev.kind)) continue;
            persisted.push_back(ev);
            state.notifications.push(std::move(ev));
        }
    }
    a.prev_snapshot = core::snapshot_from(new_bans, new_cs2);
    a.prev_snapshot.snapshot_unix = now;
    return persisted;
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

// Immediate one-shot balloon for a single manual refresh. Gated by the
// Windows-notification setting and suppressed while the main window is focused
// (the in-app toast already covers that case).
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

// Records events from a batch refresh so they can be coalesced into one balloon.
void note_session_event(AppState& state, const core::Account& a,
                        const std::vector<core::BanEvent>& events) {
    if (events.empty()) return;
    if (state.session_event_count == 0) {
        state.session_event_message = toast_message_for(a, events.front());
        state.session_event_warning = ban_event_is_cooldown(events.front().kind);
    }
    state.session_event_count += static_cast<int>(events.size());
}

// Shows the accumulated batch events as a single balloon, then clears the
// accumulator. No-op if nothing accumulated or the window is focused.
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
}  // namespace

VaultSaver::~VaultSaver() {
    {
        std::lock_guard lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void VaultSaver::start(std::filesystem::path path) {
    std::lock_guard lk(mtx_);
    if (started_) return;
    path_ = std::move(path);
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

void VaultSaver::schedule(const core::Vault& vault,
                           const crypto::SecureString& password) {
    {
        std::lock_guard lk(mtx_);
        pending_ = Pending{vault, password,
                            std::chrono::steady_clock::now() + kSaveDebounce};
    }
    cv_.notify_all();
}

void VaultSaver::flush() {
    std::unique_lock lk(mtx_);
    if (!started_) return;
    if (pending_.has_value()) {
        // Pull the deadline forward so the worker fires immediately.
        pending_->deadline = std::chrono::steady_clock::now();
        cv_.notify_all();
    }
    cv_.wait(lk, [&] { return !pending_.has_value() && !busy_; });
}

void VaultSaver::run() {
    while (true) {
        std::optional<Pending> work;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [&] { return stop_ || pending_.has_value(); });

            // Coalesce: wait for the deadline, allowing further schedule() calls
            // to push it. wait_until returns spuriously, which is what we want.
            while (!stop_ && pending_.has_value() &&
                   pending_->deadline > std::chrono::steady_clock::now()) {
                cv_.wait_until(lk, pending_->deadline);
            }

            if (pending_.has_value()) {
                work = std::move(pending_);
                pending_.reset();
                busy_ = true;
            } else if (stop_) {
                break;
            }
        }

        if (work) {
            try {
                core::store::save_vault(path_, work->vault, work->password);
            } catch (const std::exception& ex) {
                SAM_LOG_ERROR("vault save (async) failed: {}", ex.what());
            }
            std::lock_guard lk(mtx_);
            busy_ = false;
            cv_.notify_all();
        }
    }

    // Drain any final pending save on shutdown so we don't lose the last edit.
    std::optional<Pending> final_work;
    {
        std::lock_guard lk(mtx_);
        if (pending_.has_value()) {
            final_work = std::move(pending_);
            pending_.reset();
        }
    }
    if (final_work) {
        try {
            core::store::save_vault(path_, final_work->vault, final_work->password);
        } catch (const std::exception& ex) {
            SAM_LOG_ERROR("vault save (shutdown drain) failed: {}", ex.what());
        }
    }
}

core::Account* AppState::find_account(const std::string& id) {
    for (auto& a : vault.accounts) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

void AppState::save_vault_if_dirty() {
    if (!vault_dirty || master_password.empty()) return;
    vault_saver.start(vault_path());
    vault_saver.schedule(vault, master_password);
    vault_dirty = false;
}

void AppState::flush_pending_save() {
    // If a UI-thread dirty flag is still set (caller forgot to call
    // save_vault_if_dirty), schedule it now so it gets flushed.
    if (vault_dirty && !master_password.empty()) {
        vault_saver.start(vault_path());
        vault_saver.schedule(vault, master_password);
        vault_dirty = false;
    }
    vault_saver.flush();
}

std::int64_t AppState::refresh_cooldown_seconds(const std::string& id) const {
    auto it = last_refresh_unix.find(id);
    if (it == last_refresh_unix.end()) return 0;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto elapsed = now - it->second;
    if (elapsed >= kMinAccountRefreshSeconds) return 0;
    return kMinAccountRefreshSeconds - elapsed;
}

std::int64_t AppState::persona_change_cooldown_seconds(const std::string& id) const {
    auto it = last_persona_change_unix.find(id);
    if (it == last_persona_change_unix.end()) return 0;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto elapsed = now - it->second;
    if (elapsed >= kPersonaChangeCooldownSeconds) return 0;
    return kPersonaChangeCooldownSeconds - elapsed;
}

void AppState::refresh_account_data() {
    if (settings.web_api_key.empty()) {
        SAM_LOG_WARN("refresh_all: no web API key");
        refresh_web_phase_done.store(true, std::memory_order_relaxed);
        return;
    }

    // Batch refresh rate limit: stops the user (or rapid re-locks) from
    // repeatedly hammering Steam at startup. The per-account GCPD scrape has
    // its own cooldown applied inside refresh_single_account.
    {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (last_batch_refresh_unix != 0 &&
            now - last_batch_refresh_unix < kMinBatchRefreshSeconds) {
            SAM_LOG_INFO("refresh_all: rate-limited ({}s since last batch)",
                         now - last_batch_refresh_unix);
            refresh_web_phase_done.store(true, std::memory_order_relaxed);
            return;
        }
        last_batch_refresh_unix = now;
    }

    // Opportunistically resolve missing steam_ids from <SteamInstall>/config/loginusers.vdf
    // so accounts added manually or via maFile can still be refreshed.
    int filled = 0;
    for (auto& a : vault.accounts) {
        if (a.steam_id_64 == 0 && !a.login.empty()) {
            auto sid = steam_local::lookup_steam_id(a.login);
            if (sid != 0) {
                a.steam_id_64 = sid;
                vault_dirty = true;
                ++filled;
            }
        }
    }
    if (filled > 0) {
        SAM_LOG_INFO("refresh_all: filled {} missing steam_ids from loginusers.vdf", filled);
    }

    std::vector<std::uint64_t> ids;
    for (const auto& a : vault.accounts) {
        if (a.steam_id_64 != 0) ids.push_back(a.steam_id_64);
    }
    if (ids.empty()) {
        SAM_LOG_WARN("refresh_all: no accounts with steam_id");
        refresh_web_phase_done.store(true, std::memory_order_relaxed);
        return;
    }

    SAM_LOG_INFO("refresh_all: fetching data for {} accounts", ids.size());

    refresh_web_phase_done.store(false, std::memory_order_relaxed);
    session_event_count = 0;
    session_event_message.clear();

    steam_api::WebApiConfig cfg;
    cfg.api_key = settings.web_api_key;
    const bool gcpd_enabled = settings.gcpd_enabled;

    job_pump::submit([cfg, ids, gcpd_enabled, this] {
        auto bans = steam_api::fetch_bans(cfg, ids);
        auto summaries = steam_api::fetch_summaries(cfg, ids);

        std::lock_guard lk(job_mutex);
        completed_jobs.push_back({"", [this, gcpd_enabled,
                                       bans = std::move(bans),
                                       summaries = std::move(summaries)] {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            int accounts_with_events = 0;
            int total_events = 0;
            std::vector<std::pair<std::string, std::vector<core::BanEvent>>> per_account_events;
            for (auto& a : vault.accounts) {
                if (a.steam_id_64 == 0) continue;
                const bool bans_changed = bans.find(a.steam_id_64) != bans.end();
                if (auto it = bans.find(a.steam_id_64); it != bans.end())
                    a.bans = it->second;
                if (auto it = summaries.find(a.steam_id_64); it != summaries.end()) {
                    a.web.persona_name = it->second.persona_name;
                    a.web.profile_url = it->second.profile_url;
                    a.web.avatar_url_full = it->second.avatar_url_full;
                    a.web.country_code = it->second.country_code;
                    a.web.created_unix = it->second.created_unix;
                    a.web.community_visibility_state = it->second.community_visibility_state;
                    a.web.profile_state = it->second.profile_state;
                    a.web.last_refreshed_unix = it->second.last_refreshed_unix;
                }
                if (bans_changed) {
                    auto evs = apply_diff_and_snapshot(*this, a, a.bans, a.cs2, now);
                    if (!evs.empty()) {
                        ++accounts_with_events;
                        total_events += static_cast<int>(evs.size());
                        per_account_events.emplace_back(a.id, std::move(evs));
                    }
                }
            }
            vault_dirty = true;
            save_vault_if_dirty();

            // Toast surface: coalesce across accounts if the batch produced
            // more than `coalesce_threshold` events; otherwise emit per-account.
            if (settings.notifications.surface_toast && accounts_with_events > 0) {
                if (accounts_with_events >= settings.notifications.coalesce_threshold) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "%d accounts have new changes - open the list to review",
                                  accounts_with_events);
                    toasts.push_summary(buf);
                } else {
                    for (const auto& [acc_id, evs] : per_account_events) {
                        if (auto* a = find_account(acc_id)) {
                            push_toasts_for(*this, *a, evs, now);
                        }
                    }
                }
            }

            // Accumulate web-side events for a single coalesced Windows balloon;
            // flushed below if no GCPD pass runs, otherwise after the last GCPD
            // account so the balloon also covers cooldown changes.
            for (const auto& [acc_id, evs] : per_account_events) {
                if (auto* a = find_account(acc_id)) note_session_event(*this, *a, evs);
            }
            refresh_web_phase_done.store(true, std::memory_order_relaxed);

            // GCPD coverage on launch: per-account scrape for any account
            // with usable session credentials, staggered ~2s apart so 30+
            // accounts don't hammer Steam in a burst.
            if (!gcpd_enabled) { flush_native_notification(*this); return; }
            std::vector<std::string> gcpd_ids;
            for (const auto& a : vault.accounts) {
                if (!a.session_id.empty() && !a.steam_login_secure.empty()) {
                    gcpd_ids.push_back(a.id);
                }
            }
            if (gcpd_ids.empty()) { flush_native_notification(*this); return; }
            refresh_all_total.store(static_cast<int>(gcpd_ids.size()),
                                    std::memory_order_relaxed);
            refresh_all_done.store(0, std::memory_order_relaxed);
            SAM_LOG_INFO("refresh_all: staggering GCPD scrape for {} accounts",
                         gcpd_ids.size());
            job_pump::submit([this, gcpd_ids = std::move(gcpd_ids)] {
                for (const auto& aid : gcpd_ids) {
                    {
                        std::lock_guard inner_lk(job_mutex);
                        completed_jobs.push_back({"", [this, aid] {
                            refresh_single_account(aid, /*batch_refresh=*/true);
                        }});
                    }
                    if (!job_pump::interruptible_sleep(std::chrono::seconds(2))) break;
                }
            });
        }});
    });
}

void AppState::refresh_accounts_staggered(std::vector<std::string> ids,
                                          std::chrono::seconds stagger) {
    if (ids.empty()) return;
    if (settings.web_api_key.empty()) {
        SAM_LOG_WARN("refresh_staggered: no web API key, skipping {} accounts",
                     ids.size());
        return;
    }
    SAM_LOG_INFO("refresh_staggered: queuing {} accounts ({}s apart)",
                 ids.size(), stagger.count());
    job_pump::submit([this, ids = std::move(ids), stagger] {
        for (const auto& aid : ids) {
            {
                std::lock_guard inner_lk(job_mutex);
                completed_jobs.push_back({"", [this, aid] {
                    refresh_single_account(aid, /*batch_refresh=*/true);
                }});
            }
            if (!job_pump::interruptible_sleep(stagger)) break;
        }
    });
}

void AppState::refresh_single_account(const std::string& id, bool batch_refresh) {
    auto* acc = find_account(id);
    if (!acc) { SAM_LOG_WARN("refresh: account '{}' not found", id); return; }
    if (settings.web_api_key.empty()) { SAM_LOG_WARN("refresh: no web API key configured"); return; }
    if (refreshing_ids.count(id)) { return; }

    // Per-account rate limit: keep the user from spamming the button and
    // tripping Steam's per-IP throttle. Workers ignore this on auto-relogin
    // (that has its own 5-minute cooldown).
    {
        auto it = last_refresh_unix.find(id);
        if (it != last_refresh_unix.end()) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now - it->second < kMinAccountRefreshSeconds) {
                SAM_LOG_INFO("refresh: rate-limited for '{}' ({}s since last)",
                             acc->login, now - it->second);
                return;
            }
        }
    }

    SAM_LOG_INFO("refresh: starting for '{}' (steam_id={})", acc->login, acc->steam_id_64);

    // If the account is missing a steam_id, try the local loginusers.vdf before
    // submitting the network job. This is a fast local file read and avoids
    // dispatching a worker just to give up.
    if (acc->steam_id_64 == 0 && !acc->login.empty()) {
        auto sid = steam_local::lookup_steam_id(acc->login);
        if (sid != 0) {
            SAM_LOG_INFO("refresh: resolved {} from loginusers.vdf for '{}'", sid, acc->login);
            acc->steam_id_64 = sid;
            vault_dirty = true;
        } else {
            SAM_LOG_WARN("refresh: '{}' not found in loginusers.vdf, cannot refresh", acc->login);
            return;
        }
    }

    refreshing_ids.insert(id);

    steam_api::WebApiConfig cfg;
    cfg.api_key = settings.web_api_key;
    const std::uint64_t resolved = acc->steam_id_64;
    std::string aid = id;

    // Capture session credentials for the GCPD scraper.
    // We rebuild a thin Account on the worker side rather than holding a pointer
    // into the vault, since the vault lives on the UI thread.
    core::Account creds;
    creds.steam_id_64 = acc->steam_id_64;
    creds.login = acc->login;
    creds.session_id = acc->session_id;
    creds.steam_login_secure = acc->steam_login_secure;
    creds.refresh_token = acc->refresh_token;
    creds.access_token = acc->access_token;
    creds.access_token_expires = acc->access_token_expires;

    // Credentials for silent auto-relogin: only sent into the worker if we
    // actually have both the password and (for Steam-Guard-enrolled accounts)
    // the shared_secret. The default_guard_provider helper inside the worker
    // refuses to do anything without a usable TOTP source.
    crypto::SecureString relogin_password = acc->password;
    std::optional<core::SteamGuardAccount> relogin_sda = acc->sda;
    const bool is_nfa = acc->is_nfa;

    bool gcpd_enabled = settings.gcpd_enabled;
    if (gcpd_enabled) {
        // GCPD scrape is heavier than the Web API batch and uses a separate
        // per-account cooldown so launch-wide refreshes don't crawl the
        // gcpd page every time.
        auto it = last_gcpd_refresh_unix.find(id);
        if (it != last_gcpd_refresh_unix.end()) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now - it->second < kMinGcpdRefreshSeconds) {
                SAM_LOG_INFO("refresh: gcpd rate-limited for '{}' ({}s since last)",
                             acc->login, now - it->second);
                gcpd_enabled = false;
            }
        }
    }

    job_pump::submit([cfg, resolved, aid, creds, relogin_password, relogin_sda,
                      gcpd_enabled, batch_refresh, is_nfa, this]() mutable {
        SAM_LOG_INFO("refresh: fetching bans/summaries/level/games for {}", resolved);
        std::vector<std::uint64_t> ids{resolved};
        auto bans = steam_api::fetch_bans(cfg, ids);
        auto summaries = steam_api::fetch_summaries(cfg, ids);
        auto level = steam_api::fetch_steam_level(cfg, resolved);
        auto games = steam_api::fetch_owned_games(cfg, resolved);

        // Web-session maintenance for the GCPD scraper:
        //  1. If we have a refresh_token and the access_token is stale, mint
        //     a fresh community-scoped access_token via GenerateAccessTokenForApp.
        //  2. If that fails (or we never had a refresh_token), attempt a
        //     silent auto-relogin using the stored password + shared_secret.
        //  3. Run transfer_login (finalizelogin + settoken) to mint a fresh
        //     steamLoginSecure cookie: these expire after ~24h even when
        //     the access_token is still valid.
        //  4. If everything fails AND no usable cookie remains, queue a UI
        //     re-login prompt as the last resort.
        bool token_refreshed = false;
        bool need_relogin = false;
        bool nfa_token_dead = false;
        std::optional<steam_gcpd::MatchmakingData> mm_parsed;
        std::optional<steam_gcpd::AccountMainData> am_parsed;

        // NFA accounts authenticate by a client-scoped refresh token, which Steam
        // refuses to exchange for a web session over HTTP (AccessDenied). Skip
        // token refresh + GCPD entirely and read liveness from the JWT itself.
        if (is_nfa) {
            const std::int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            nfa_token_dead = creds.refresh_token.empty() ||
                steam_login::jwt_expiry(creds.refresh_token) <= now_s ||
                steam_login::jwt_audience(creds.refresh_token).find("client")
                    == std::string::npos;
        }

        if (!is_nfa) {
        // Silent auto-relogin. Returns true iff we now hold a fresh
        // refresh_token + access_token from a successful credentials login.
        // Rate-limited to one attempt / 5 min / account so a bad stored
        // password can't pin Steam's IP rate-limiter.
        auto try_auto_relogin = [&]() -> bool {
            if (relogin_password.empty()) {
                SAM_LOG_INFO("auto-relogin: skipped for '{}' (no stored password)", creds.login);
                return false;
            }
            {
                std::lock_guard lk(this->relogin_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto it = this->last_relogin_attempt.find(aid);
                if (it != this->last_relogin_attempt.end() &&
                    (now - it->second) < std::chrono::minutes(5)) {
                    const auto since = std::chrono::duration_cast<std::chrono::seconds>(
                        now - it->second).count();
                    SAM_LOG_INFO("auto-relogin: cooldown active for '{}' ({}s since last attempt)",
                                 creds.login, since);
                    return false;
                }
                this->last_relogin_attempt[aid] = now;
            }

            SAM_LOG_INFO("auto-relogin: attempting for '{}'", creds.login);
            steam_login::MobileLogin login;
            login.username = creds.login;
            login.password = relogin_password;

            auto result = steam_login::run_full_login(
                login,
                [&](const std::vector<steam_login::GuardKind>& allowed) {
                    // No UI prompt available from a worker thread; only the
                    // shared_secret TOTP path can succeed silently.
                    return steam_login::default_guard_provider(
                        relogin_sda, allowed, /*on_prompt=*/{});
                },
                [&](const std::string& s) {
                    SAM_LOG_INFO("auto-relogin: {} ({})", s, creds.login);
                });

            if (!result.ok) {
                SAM_LOG_WARN("auto-relogin: failed for '{}': {}", creds.login, result.error);
                return false;
            }
            SAM_LOG_INFO("auto-relogin: succeeded for '{}'", creds.login);
            creds.refresh_token        = std::move(result.account.refresh_token);
            creds.access_token         = std::move(result.account.access_token);
            creds.access_token_expires = result.account.access_token_expires;
            // run_full_login already registered the community session via
            // finalizelogin + settoken; mint_cookie below becomes a no-op
            // for this call (the cookie is fresh) but is still load-bearing
            // for the steady-state 24h-expiry refresh path.
            creds.steam_login_secure   = std::move(result.account.steam_login_secure);
            if (!result.account.session_id.empty()) {
                creds.session_id = std::move(result.account.session_id);
            } else if (creds.session_id.empty()) {
                creds.session_id = crypto::random_session_id();
            }
            return true;
        };

        // Ensure we have a fresh access_token.
        if (creds.refresh_token.empty()) {
            SAM_LOG_INFO("refresh: no refresh_token for '{}', trying auto-relogin", creds.login);
            if (try_auto_relogin()) token_refreshed = true;
            else                    need_relogin    = true;
        } else if (steam_login::needs_refresh(creds, 300)) {
            if (steam_login::refresh_access_token(creds)) {
                token_refreshed = true;
            } else {
                SAM_LOG_WARN("refresh: GenerateAccessTokenForApp failed for '{}', refresh_token is "
                             "invalid, expired, or wrong scope; trying auto-relogin", creds.login);
                if (try_auto_relogin()) token_refreshed = true;
                else                    need_relogin    = true;
            }
        }

        // Mint a fresh community-domain steamLoginSecure cookie.
        // If transfer_login fails (typically because the access_token was
        // minted from an old wrong-audience refresh_token), try one
        // auto-relogin to get a token whose `aud` includes web:community.
        auto mint_cookie = [&]() -> bool {
            if (creds.refresh_token.empty() || creds.session_id.empty()) return false;
            std::string new_cookie;
            // finalizelogin takes the refresh_token (not the access_token)
            // as nonce: see session.hpp.
            if (steam_login::transfer_login(creds.steam_id_64,
                                            creds.refresh_token,
                                            creds.session_id,
                                            new_cookie)) {
                creds.steam_login_secure = crypto::make_secure(new_cookie);
                return true;
            }
            return false;
        };

        if (!need_relogin) {
            if (mint_cookie()) {
                token_refreshed = true;
            } else {
                SAM_LOG_WARN("refresh: transfer_login failed for '{}', trying auto-relogin", creds.login);
                if (try_auto_relogin() && mint_cookie()) {
                    token_refreshed = true;
                } else if (creds.steam_login_secure.empty()) {
                    need_relogin = true;
                }
            }
        }

        // GCPD HTML scraper: populates premier_rating, wingman_rank,
        // cooldown_* from matchmaking; CS2 player level + XP from
        // accountmain (prime_status is inferred from these, see merge).
        const bool gcpd_creds_ok = !creds.session_id.empty() && !creds.steam_login_secure.empty();
        if (gcpd_enabled && gcpd_creds_ok) {
            // Session-expired detection is content-based, not status-based:
            // Steam 302-rewrites `/profiles/{steamid}/gcpd/730` to
            // `/id/{vanity}/gcpd/730` for any account with a vanity URL,
            // which is the common case. The scraper follows redirects;
            // here we check the body shape.
            auto run_tab = [&](steam_gcpd::Tab tab, auto parser) {
                const auto resp = steam_gcpd::fetch_gcpd(creds, tab);
                if (resp.status != 200) {
                    SAM_LOG_WARN("gcpd: tab fetch http {} for '{}' (no body parse)",
                                 resp.status, creds.login);
                    return;
                }
                if (steam_gcpd::looks_like_login_page(resp.body)) {
                    SAM_LOG_WARN("gcpd: response is login page for '{}', session expired",
                                 creds.login);
                    need_relogin = true;
                    return;
                }
                if (!steam_gcpd::looks_like_gcpd_page(resp.body)) {
                    SAM_LOG_WARN("gcpd: response for '{}' looks unfamiliar ({} bytes), skipping parse",
                                 creds.login, resp.body.size());
                    return;
                }
                parser(resp.body);
            };
            run_tab(steam_gcpd::Tab::Matchmaking, [&](const std::string& body) {
                mm_parsed = steam_gcpd::parse_matchmaking(body);
            });
            run_tab(steam_gcpd::Tab::AccountMain, [&](const std::string& body) {
                am_parsed = steam_gcpd::parse_accountmain(body);
            });
        } else if (gcpd_enabled) {
            SAM_LOG_INFO("gcpd: skipped for {} (session_set={} login_secure_set={})",
                         resolved, !creds.session_id.empty(),
                         !creds.steam_login_secure.empty());
        }
        }  // if (!is_nfa)

        // Snapshot any refreshed-token values so the UI thread can update the
        // vault Account. Copy SecureStrings out: they're cheap.
        crypto::SecureString refreshed_at, refreshed_rt, refreshed_ls;
        std::string refreshed_sid;
        std::int64_t refreshed_exp = 0;
        if (token_refreshed) {
            refreshed_at = creds.access_token;
            refreshed_rt = creds.refresh_token;       // GenerateAccessTokenForApp can rotate this
            refreshed_ls = creds.steam_login_secure;
            // Auto-relogin can mint a fresh session_id when the vault was missing one.
            // Persist it so the next refresh hits mint_cookie() instead of getting
            // stuck on the 5-minute auto-relogin cooldown.
            refreshed_sid = creds.session_id;
            refreshed_exp = creds.access_token_expires;
        }
        std::string relogin_login;
        if (need_relogin) {
            // Launch/batch refresh can never silently log a non-SDA account back in
            // (default_guard_provider has no shared_secret to satisfy the mobile
            // guard, and there's no prompt callback from a worker thread). Suppress
            // the popup in this case so launch doesn't shove a Full Login wizard at
            // the user. SDA accounts keep the current behaviour as a fallback.
            // Manual single-account refresh (batch_refresh=false) is unchanged.
            if (batch_refresh && !relogin_sda.has_value()) {
                SAM_LOG_INFO("refresh: skipping launch login prompt for non-SDA '{}'",
                             creds.login);
            } else {
                relogin_login = creds.login;
            }
        }

        std::lock_guard lk(job_mutex);
        completed_jobs.push_back({"", [this, aid, resolved, batch_refresh,
                                       bans = std::move(bans),
                                       summaries = std::move(summaries),
                                       level, games,
                                       mm_parsed = std::move(mm_parsed),
                                       am_parsed = std::move(am_parsed),
                                       token_refreshed, nfa_token_dead,
                                       refreshed_at = std::move(refreshed_at),
                                       refreshed_rt = std::move(refreshed_rt),
                                       refreshed_ls = std::move(refreshed_ls),
                                       refreshed_sid = std::move(refreshed_sid),
                                       refreshed_exp,
                                       relogin_login = std::move(relogin_login)] {
            refreshing_ids.erase(aid);
            auto* a = find_account(aid);
            if (!a) return;
            if (a->steam_id_64 == 0) a->steam_id_64 = resolved;
            if (token_refreshed) {
                a->access_token         = refreshed_at;
                a->refresh_token        = refreshed_rt;
                a->steam_login_secure   = refreshed_ls;
                a->access_token_expires = refreshed_exp;
                if (!refreshed_sid.empty()) a->session_id = refreshed_sid;
            }
            if (auto it = bans.find(resolved); it != bans.end())
                a->bans = it->second;
            if (auto it = summaries.find(resolved); it != summaries.end()) {
                a->web.persona_name = it->second.persona_name;
                a->web.profile_url = it->second.profile_url;
                a->web.avatar_url_full = it->second.avatar_url_full;
                a->web.country_code = it->second.country_code;
                a->web.created_unix = it->second.created_unix;
                a->web.community_visibility_state = it->second.community_visibility_state;
                a->web.profile_state = it->second.profile_state;
                a->web.last_refreshed_unix = it->second.last_refreshed_unix;
            }
            if (level.has_value()) a->web.steam_level = *level;
            if (games.has_value()) {
                a->web.owned_games_count = games->games_count;
                a->web.total_playtime_minutes = games->total_playtime_minutes;
            }

            bool cs2_touched = false;
            if (mm_parsed && mm_parsed->ok) {
                if (mm_parsed->premier_rating > 0) a->cs2.premier_rating = mm_parsed->premier_rating;
                if (mm_parsed->premier_wins  >= 0) a->cs2.premier_wins   = mm_parsed->premier_wins;
                if (mm_parsed->wingman_rank   > 0) a->cs2.wingman_rank   = mm_parsed->wingman_rank;
                if (mm_parsed->wingman_wins  >= 0) a->cs2.wingman_wins   = mm_parsed->wingman_wins;
                a->cs2.cooldown_expires_unix = mm_parsed->cooldown_expires_unix;
                a->cs2.cooldown_reason       = mm_parsed->cooldown_reason;
                a->cs2.vac_live              = a->bans.vac_banned;
                cs2_touched = true;
            }
            if (am_parsed && am_parsed->ok) {
                if (am_parsed->cs2_player_level >= 0) {
                    a->cs2.cs2_player_level = am_parsed->cs2_player_level;
                }
                if (am_parsed->cs2_player_xp >= 0) {
                    a->cs2.cs2_player_xp = am_parsed->cs2_player_xp;
                }
                // Prime inference: in CS2 you can't earn XP on a non-Prime
                // account. So if level > 1, or level == 1 with any XP earned,
                // the account has Prime. The dedicated `primeaccount` GCPD
                // tab is no longer reliable for this signal in CS2.
                const bool level_above_1 = am_parsed->cs2_player_level > 1;
                const bool has_any_xp    = am_parsed->cs2_player_xp > 0;
                a->cs2.prime_status = level_above_1 || has_any_xp;
                cs2_touched = true;
            }

            const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (cs2_touched) {
                a->cs2.last_refreshed_unix = now_unix;
                last_gcpd_refresh_unix[aid] = now_unix;
            }
            last_refresh_unix[aid] = now_unix;

            const bool bans_arrived = bans.find(resolved) != bans.end();
            if (bans_arrived || cs2_touched) {
                auto evs = apply_diff_and_snapshot(*this, *a, a->bans, a->cs2, now_unix);
                if (!evs.empty()) {
                    if (batch_refresh) {
                        note_session_event(*this, *a, evs);
                    } else {
                        push_toasts_for(*this, *a, evs, now_unix);
                        push_native_notification(*this, toast_message_for(*a, evs.front()),
                                                 ban_event_is_cooldown(evs.front().kind));
                    }
                }
            }
            if (batch_refresh) {
                const int done = refresh_all_done.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done >= refresh_all_total.load(std::memory_order_relaxed)) {
                    flush_native_notification(*this);
                }
            }

            if (!relogin_login.empty() && !pending_relogin_login.has_value()) {
                SAM_LOG_INFO("refresh: queuing UI re-login prompt for '{}'", relogin_login);
                pending_relogin_login = relogin_login;
            }

            // NFA token expired/invalid: notify once per dead token. Detection is
            // from the JWT (not an API result), so it can't false-positive on the
            // expected AccessDenied. Cleared on re-import so a fresh token re-arms.
            if (nfa_token_dead && nfa_dead_notified.insert(aid).second) {
                const std::string who = a->web.persona_name.empty()
                                            ? a->login : a->web.persona_name;
                const std::string msg = "NFA token expired - " + who +
                                        " (re-import a refresh token)";
                ui::widgets::ToastItem t;
                t.id = "nfa-dead-" + aid;
                t.message = msg;
                t.account_id = aid;
                t.is_warning = true;
                t.expires_at_unix = now_unix + settings.notifications.toast_duration_seconds;
                toasts.push(std::move(t));
                push_native_notification(*this, msg, true);
            }

            SAM_LOG_INFO("refresh: done for '{}', bans={} summaries={} cs2_touched={}",
                         aid, bans.size(), summaries.size(), cs2_touched);
            vault_dirty = true;
            save_vault_if_dirty();
        }});
    });
}

bool AppState::auto_relogin(const std::string& account_id,
                            core::Account& creds,
                            std::string* err) {
    auto set_err = [&](std::string msg) { if (err) *err = std::move(msg); };

    if (creds.login.empty() || creds.password.empty()) {
        SAM_LOG_INFO("auto-relogin: skipped for '{}' (no stored password)", creds.login);
        set_err("no stored password");
        return false;
    }

    {
        std::lock_guard lk(relogin_mutex);
        const auto now = std::chrono::steady_clock::now();
        const auto it = last_relogin_attempt.find(account_id);
        if (it != last_relogin_attempt.end() &&
            (now - it->second) < std::chrono::minutes(5)) {
            const auto since = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second).count();
            SAM_LOG_INFO("auto-relogin: cooldown active for '{}' ({}s since last attempt)",
                         creds.login, since);
            set_err("login rate-limited");
            return false;
        }
        last_relogin_attempt[account_id] = now;
    }

    SAM_LOG_INFO("auto-relogin: attempting for '{}'", creds.login);
    steam_login::MobileLogin login;
    login.username = creds.login;
    login.password = creds.password;

    auto relogin_sda = creds.sda;
    auto result = steam_login::run_full_login(
        login,
        [&](const std::vector<steam_login::GuardKind>& allowed) {
            return steam_login::default_guard_provider(
                relogin_sda, allowed, /*on_prompt=*/{});
        },
        [&](const std::string& s) {
            SAM_LOG_INFO("auto-relogin: {} ({})", s, creds.login);
        });

    if (!result.ok) {
        SAM_LOG_WARN("auto-relogin: failed for '{}': {}", creds.login, result.error);
        set_err(result.error.empty() ? std::string{"steam declined"} : result.error);
        return false;
    }

    SAM_LOG_INFO("auto-relogin: succeeded for '{}'", creds.login);
    creds.refresh_token        = std::move(result.account.refresh_token);
    creds.access_token         = std::move(result.account.access_token);
    creds.access_token_expires = result.account.access_token_expires;
    // run_full_login runs finalizelogin + settoken internally and ships
    // back both steam_login_secure and session_id bound to the registered
    // community session. The cookie may be the manual fallback if settoken
    // failed; mobileconf writes will then trip success=false until the
    // next relogin retries the registration.
    creds.steam_login_secure   = std::move(result.account.steam_login_secure);
    if (!result.account.session_id.empty()) {
        creds.session_id = std::move(result.account.session_id);
    } else if (creds.session_id.empty()) {
        creds.session_id = crypto::random_session_id();
    }
    return true;
}

void AppState::post_ui_callback(std::function<void()> fn) {
    std::lock_guard lk(job_mutex);
    completed_jobs.push_back({"", std::move(fn)});
}

std::int64_t AppState::relogin_cooldown_seconds(const std::string& account_id) {
    std::lock_guard lk(relogin_mutex);
    const auto it = last_relogin_attempt.find(account_id);
    if (it == last_relogin_attempt.end()) return 0;
    const auto elapsed = std::chrono::steady_clock::now() - it->second;
    const auto window = std::chrono::minutes(5);
    if (elapsed >= window) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(window - elapsed).count();
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
    j["start_with_windows"]      = settings.start_with_windows;
    j["privacy_mode"]            = settings.privacy_mode;
    j["web_api_key"]             = settings.web_api_key;
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
    lv["show_cooldown_marker"] = settings.list_view.show_cooldown_marker;
    lv["show_unread_badge"]    = settings.list_view.show_unread_badge;
    lv["hide_account_name"]    = settings.list_view.hide_account_name;

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

    j["accounts_sort"] = settings.accounts_sort;
    j["collapsed_groups"] = settings.collapsed_groups;
    auto& qf = j["quick_filters"];
    qf["only_banned"]   = settings.quick_filters.only_banned;
    qf["only_cooldown"] = settings.quick_filters.only_cooldown;
    qf["only_prime"]    = settings.quick_filters.only_prime;

    auto& vj = j["cs2_video"];
    vj["auto_apply_on_login"] = settings.cs2_video.auto_apply_on_login;
    vj["source_label"]        = settings.cs2_video.source_label;

    auto path = settings_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (out) {
        out << j.dump(4);
    }
}

void AppState::load_settings() {
    // A settings reload is a lock-equivalent transition: any privacy_mode
    // reveals from before the reload should not survive it.
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
    get("start_with_windows",      settings.start_with_windows);
    get("privacy_mode",            settings.privacy_mode);
    get("web_api_key",             settings.web_api_key);
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
        get_l("show_cooldown_marker", settings.list_view.show_cooldown_marker);
        get_l("show_unread_badge",    settings.list_view.show_unread_badge);
        get_l("hide_account_name",    settings.list_view.hide_account_name);
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
        get_v("auto_apply_on_login", settings.cs2_video.auto_apply_on_login);
        get_v("source_label",        settings.cs2_video.source_label);
    }
}

void AppState::apply_cs2_video_config(const core::Account& a) {
    const auto result =
        cs2_config::deploy_video_config(a.steam_id_64, cs2_video_template_path());

    const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ui::widgets::ToastItem t;
    t.id = "cs2video-" + a.id + "-" + std::to_string(now_unix);
    t.message = result.ok ? "CS2 video config applied"
                          : ("CS2 video config: " + result.message);
    t.account_id = a.id;
    t.is_warning = !result.ok;
    t.expires_at_unix = now_unix + settings.notifications.toast_duration_seconds;
    toasts.push(std::move(t));

    if (!result.ok) {
        SAM_LOG_WARN("cs2 video config: {} (login={})", result.message, a.login);
    }
}

void AppState::clear_session_secrets() {
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
    for (auto& a : vault.accounts) scrub_account_secrets(a);
}

void AppState::remove_accounts(std::unordered_set<std::string> ids) {
    if (ids.empty()) return;
    auto& accs = vault.accounts;
    for (auto& a : accs) {
        if (ids.count(a.id) != 0) scrub_account_secrets(a);
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
