#include "ui/screens/confirmations_screen.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/job_pump.hpp"
#include "core/log.hpp"
#include "core/sda/confirmation.hpp"
#include "core/sda/mafile.hpp"
#include "platform/file_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/search_bar.hpp"

namespace sam::ui::screens {

namespace {

constexpr float kCardMinWidth = 280.0F;
constexpr float kCardHeight   = 170.0F;
constexpr float kGap          = 14.0F;
constexpr float kGridInset    = 10.0F;
constexpr float kButtonRowH   = 40.0F;
constexpr float kIconSize     = 48.0F;
constexpr float kPillWidth    = 72.0F;
constexpr float kBulkButtonW  = 140.0F;

struct PendingList {
    std::vector<sda::Confirmation> items;
    bool refreshing = false;
    std::string last_error;
};

std::mutex g_mtx;
std::unordered_map<std::string, PendingList> g_lists;

bool session_looks_unusable(const core::Account& a) {
    return a.session_id.empty() || a.access_token.empty() || a.steam_id_64 == 0;
}

bool is_session_error(const std::string& err) {
    if (err.empty()) return false;
    // Relogin can't backfill identity_secret or device_id; only a maFile can.
    if (err.find("missing maFile data") != std::string::npos) return false;
    if (err == "account has no active session") return true;
    if (err == "http 401" || err == "http 403") return true;
    if (err.find("session expired") != std::string::npos) return true;
    if (err.find("needauth")        != std::string::npos) return true;

    // /mobileconf/* literally returns "Oh nooooooes!" when the
    // steamLoginSecure cookie has the wrong audience or the HMAC is bad.
    // Either way, a fresh mobile-audience auto-relogin is the right recovery.
    std::string lower(err);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower.find("oh nooooooes") != std::string::npos) return true;

    // `{"success":false}` with no message: caller has already tried a refetch
    // for stale cid/nonce. The remaining likely cause is the access_token
    // having drifted off the mobile audience, so trigger auto-relogin.
    if (err == sda::kRespondSuccessFalse) return true;

    return false;
}

void apply_refreshed_tokens(app::AppState& state, const core::Account& creds) {
    state.post_ui_callback([&state, aid = creds.id,
                            rt  = creds.refresh_token,
                            at  = creds.access_token,
                            exp = creds.access_token_expires,
                            ls  = creds.steam_login_secure,
                            sid = creds.session_id] {
        auto* a = state.find_account(aid);
        if (!a) return;
        a->refresh_token        = rt;
        a->access_token         = at;
        a->access_token_expires = exp;
        a->steam_login_secure   = ls;
        a->session_id           = sid;
        state.vault_dirty = true;
        state.save_vault_if_dirty();
    });
}

struct ScanResult {
    int targeted = 0;
    int linked = 0;
    int skipped_encrypted = 0;
    int skipped_no_match = 0;
    int skipped_ambiguous = 0;
    int parse_errors = 0;
};

std::atomic<bool> g_scan_in_progress{false};
bool g_show_scan_modal = false;
ScanResult g_last_scan;

bool needs_mafile_data(const core::Account& a) {
    if (!a.sda.has_value()) return false;
    return a.sda->identity_secret.empty() || a.sda->device_id.empty();
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

void merge_guard_into_account(core::Account& a, const core::SteamGuardAccount& g) {
    if (!a.sda.has_value()) a.sda = core::SteamGuardAccount{};
    auto& s = *a.sda;
    if (!g.shared_secret.empty())   s.shared_secret   = g.shared_secret;
    if (!g.serial_number.empty())   s.serial_number   = g.serial_number;
    if (!g.revocation_code.empty()) s.revocation_code = g.revocation_code;
    if (!g.uri.empty())             s.uri             = g.uri;
    if (g.server_time != 0)         s.server_time     = g.server_time;
    if (s.account_name.empty())     s.account_name    = g.account_name;
    if (!g.token_gid.empty())       s.token_gid       = g.token_gid;
    if (!g.identity_secret.empty()) s.identity_secret = g.identity_secret;
    if (!g.secret_1.empty())        s.secret_1        = g.secret_1;
    if (!g.device_id.empty())       s.device_id       = g.device_id;
    s.status         = g.status;
    s.fully_enrolled = g.fully_enrolled;
}

bool mafile_matches_account(const sam::sda::MafileLoadResult& loaded,
                             const std::string& login_lower,
                             std::uint64_t steam_id_64) {
    if (loaded.session_steam_id != 0 && steam_id_64 != 0 &&
        loaded.session_steam_id == steam_id_64) {
        return true;
    }
    if (!loaded.guard.account_name.empty()) {
        return to_lower(loaded.guard.account_name) == login_lower;
    }
    return false;
}

void submit_account_refresh(app::AppState& state, const core::Account& seed);
void recount_pending(app::AppState& state);
void submit_single(app::AppState& state, const core::Account& a,
                   const sda::Confirmation& c, bool allow);
void erase_item(app::AppState& state, const std::string& account_id,
                const std::string& conf_id);

bool should_auto_approve(const app::AppState& state, const sda::Confirmation& c) {
    const auto& cfg = state.settings.confirmations;
    if (!cfg.auto_approve_enabled) return false;
    switch (c.type) {
        case sda::ConfirmationType::MarketListing:
            return cfg.auto_approve_market;
        case sda::ConfirmationType::PhoneNumberChange:
            return cfg.auto_approve_phone_change;
        default:
            return false;
    }
}

void link_mafile_for_account(app::AppState& state, const core::Account& a) {
    platform::file_dialog::Options opts;
    opts.parent = state.main_hwnd;
    opts.title = L"Choose maFile for this account";
    opts.filters = {
        {L"Steam mobile authenticator (*.maFile)", L"*.maFile"},
        {L"All files (*.*)", L"*.*"},
    };
    const auto picked = platform::file_dialog::open_file(opts);
    if (!picked.ok) return;

    const std::string aid = a.id;
    const std::string login_lower = to_lower(a.login);
    const std::uint64_t sid = a.steam_id_64;
    const std::filesystem::path path = picked.path;

    {
        std::lock_guard lk(g_mtx);
        g_lists[aid].refreshing = true;
        g_lists[aid].last_error.clear();
    }

    app::job_pump::submit([&state, aid, login_lower, sid, path]() mutable {
        sam::sda::MafileLoadResult loaded;
        try {
            loaded = sam::sda::load_mafile(path, {});
        } catch (const sam::sda::MafileEncrypted&) {
            std::lock_guard lk(g_mtx);
            g_lists[aid].refreshing = false;
            g_lists[aid].last_error = "maFile is encrypted; decrypt it first";
            return;
        } catch (const std::exception& ex) {
            std::lock_guard lk(g_mtx);
            g_lists[aid].refreshing = false;
            g_lists[aid].last_error = std::string{"maFile load failed: "} + ex.what();
            return;
        }

        if (!mafile_matches_account(loaded, login_lower, sid)) {
            std::lock_guard lk(g_mtx);
            g_lists[aid].refreshing = false;
            g_lists[aid].last_error =
                "maFile does not match this account (got '" +
                loaded.guard.account_name + "')";
            return;
        }

        state.post_ui_callback([&state, aid, guard = std::move(loaded.guard)]() mutable {
            auto* acc = state.find_account(aid);
            if (!acc) return;
            merge_guard_into_account(*acc, guard);
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            SAM_LOG_INFO("confirmation: linked maFile for '{}'", acc->login);

            core::Account snap = *acc;
            {
                std::lock_guard lk(g_mtx);
                g_lists[aid].refreshing = false;
                g_lists[aid].last_error.clear();
            }
            submit_account_refresh(state, snap);
        });
    });
}

void scan_folder_for_mafiles(app::AppState& state) {
    if (g_scan_in_progress.load()) return;

    platform::file_dialog::Options opts;
    opts.parent = state.main_hwnd;
    opts.title = L"Pick folder containing maFiles";
    const auto picked = platform::file_dialog::pick_folder(opts);
    if (!picked.ok) return;

    struct Target {
        std::string id;
        std::string login_lower;
        std::string steam_id_str;
        std::uint64_t steam_id_64 = 0;
    };
    std::vector<Target> targets;
    for (const auto& a : state.vault.accounts) {
        if (!needs_mafile_data(a)) continue;
        Target t;
        t.id = a.id;
        t.login_lower = to_lower(a.login);
        t.steam_id_64 = a.steam_id_64;
        if (t.steam_id_64 != 0) t.steam_id_str = std::to_string(t.steam_id_64);
        targets.push_back(std::move(t));
    }

    if (targets.empty()) {
        std::lock_guard lk(g_mtx);
        g_last_scan = ScanResult{};
        g_show_scan_modal = true;
        return;
    }

    g_scan_in_progress.store(true);
    const int targeted_count = static_cast<int>(targets.size());
    const std::filesystem::path folder = picked.path;
    SAM_LOG_INFO("confirmation: scanning '{}' for {} missing accounts",
                 folder.string(), targeted_count);

    app::job_pump::submit([&state, folder, targets = std::move(targets),
                            targeted_count]() mutable {
        ScanResult res;
        res.targeted = targeted_count;

        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            folder, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator end;

        for (; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) { ec.clear(); continue; }
            const auto& path = it->path();
            const std::string ext = to_lower(path.extension().string());
            if (ext != ".mafile") continue;

            const std::string stem = to_lower(path.stem().string());

            int candidate = -1;
            int matches = 0;
            for (std::size_t i = 0; i < targets.size(); ++i) {
                if (!targets[i].steam_id_str.empty() &&
                    stem.find(targets[i].steam_id_str) != std::string::npos) {
                    candidate = static_cast<int>(i);
                    ++matches;
                }
            }
            if (matches == 0) {
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    if (targets[i].login_lower.empty()) continue;
                    if (stem.find(targets[i].login_lower) != std::string::npos) {
                        candidate = static_cast<int>(i);
                        ++matches;
                    }
                }
            }
            if (matches == 0) continue;
            if (matches > 1) {
                ++res.skipped_ambiguous;
                continue;
            }

            sam::sda::MafileLoadResult loaded;
            try {
                loaded = sam::sda::load_mafile(path, {});
            } catch (const sam::sda::MafileEncrypted&) {
                ++res.skipped_encrypted;
                continue;
            } catch (...) {
                ++res.parse_errors;
                continue;
            }

            const auto& tgt = targets[candidate];
            if (!mafile_matches_account(loaded, tgt.login_lower, tgt.steam_id_64)) {
                ++res.skipped_no_match;
                continue;
            }
            if (loaded.guard.identity_secret.empty() ||
                loaded.guard.device_id.empty()) {
                ++res.skipped_no_match;
                continue;
            }

            const std::string aid = tgt.id;
            state.post_ui_callback(
                [&state, aid, guard = std::move(loaded.guard)]() mutable {
                    auto* acc = state.find_account(aid);
                    if (!acc) return;
                    merge_guard_into_account(*acc, guard);
                    state.vault_dirty = true;
                    std::lock_guard lk(g_mtx);
                    if (g_lists[aid].last_error.find("missing maFile data") !=
                        std::string::npos) {
                        g_lists[aid].last_error.clear();
                    }
                });
            ++res.linked;
            targets.erase(targets.begin() + candidate);
            if (targets.empty()) break;
        }

        state.post_ui_callback([&state, res] {
            state.save_vault_if_dirty();
            std::lock_guard lk(g_mtx);
            g_last_scan = res;
            g_show_scan_modal = true;
            SAM_LOG_INFO("confirmation: scan done, linked {}/{} (skipped: "
                         "encrypted={} no_match={} ambiguous={} parse_errors={})",
                         res.linked, res.targeted,
                         res.skipped_encrypted, res.skipped_no_match,
                         res.skipped_ambiguous, res.parse_errors);
        });
        g_scan_in_progress.store(false);
    });
}

void submit_account_refresh(app::AppState& state, const core::Account& seed) {
    if (session_looks_unusable(seed) && seed.password.empty()) {
        std::lock_guard lk(g_mtx);
        auto& pl = g_lists[seed.id];
        pl.refreshing = false;
        pl.last_error = "no stored password, open Add Account and run the full login";
        return;
    }

    const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto cooldown = state.settings.confirmations.per_account_cooldown_seconds;
    if (cooldown > 0) {
        const auto it = state.conf_last_refresh_unix.find(seed.id);
        if (it != state.conf_last_refresh_unix.end() &&
            now_unix - it->second < cooldown) {
            return;
        }
    }

    {
        std::lock_guard lk(g_mtx);
        auto& pl = g_lists[seed.id];
        if (pl.refreshing) return;
        pl.refreshing = true;
        pl.last_error.clear();
    }
    const auto snapshot = seed;
    app::job_pump::submit([&state, snapshot]() mutable {
        core::Account creds = snapshot;
        sda::ConfirmationFetchResult result;

        const bool need_session = session_looks_unusable(creds);
        if (!need_session) {
            result = sda::fetch_confirmations(creds);
        }

        if (need_session || (!result.ok && is_session_error(result.error))) {
            const auto cd = state.relogin_cooldown_seconds(creds.id);
            if (cd > 0) {
                result.ok = false;
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "login rate-limited, try again in %lld s",
                              static_cast<long long>(cd));
                result.error = buf;
                result.items.clear();
            } else {
                std::string relogin_err;
                if (state.auto_relogin(creds.id, creds, &relogin_err)) {
                    apply_refreshed_tokens(state, creds);
                    result = sda::fetch_confirmations(creds);
                } else {
                    result.ok = false;
                    result.error = "auto-relogin failed: " + relogin_err;
                    result.items.clear();
                }
            }
        }

        const auto done_unix = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const bool ok = result.ok;
        const bool session_err = !ok && is_session_error(result.error);
        {
            std::lock_guard lk(g_mtx);
            auto& pl = g_lists[creds.id];
            pl.refreshing = false;
            if (ok) {
                pl.items = std::move(result.items);
                pl.last_error.clear();
            } else {
                pl.last_error = result.error;
            }
        }
        state.post_ui_callback([&state, aid = creds.id, done_unix, ok, session_err] {
            state.conf_last_refresh_unix[aid] = done_unix;
            const int done = state.conf_refresh_all_done.fetch_add(
                1, std::memory_order_relaxed) + 1;
            const int total = state.conf_refresh_all_total.load(std::memory_order_relaxed);
            if (ok) {
                state.conf_consecutive_failures.erase(aid);
                state.conf_permanent_failure.erase(aid);

                if (auto* a = state.find_account(aid)) {
                    std::vector<sda::Confirmation> auto_picks;
                    {
                        std::lock_guard lk(g_mtx);
                        for (const auto& c : g_lists[aid].items) {
                            if (should_auto_approve(state, c)) auto_picks.push_back(c);
                        }
                    }
                    for (const auto& c : auto_picks) {
                        submit_single(state, *a, c, true);
                        sda::AuditEntry e;
                        e.unix_time = done_unix;
                        e.account_id = a->id;
                        e.account_login = a->login;
                        e.conf_id = c.id;
                        e.conf_type = c.type;
                        e.headline = c.headline;
                        e.allow = true;
                        e.source = sda::AuditSource::Auto;
                        state.conf_audit.record(std::move(e));
                        erase_item(state, a->id, c.id);
                    }
                }
            } else if (session_err) {
                const int n = ++state.conf_consecutive_failures[aid];
                if (n >= state.settings.confirmations.permanent_failure_threshold) {
                    state.conf_permanent_failure.insert(aid);
                }
            }
            if (total > 0 && done >= total) {
                const int baseline = state.conf_refresh_batch_baseline.load(
                    std::memory_order_relaxed);
                const int now_count = state.pending_confirmations_count.load(
                    std::memory_order_relaxed);
                const int delta = now_count - baseline;
                if (delta > 0 &&
                    state.settings.confirmations.toast_on_new_confirmations) {
                    ui::widgets::ToastItem t;
                    t.id = "conf-new-" + std::to_string(done_unix);
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "%d new confirmation%s",
                                  delta, delta == 1 ? "" : "s");
                    t.message = buf;
                    t.is_warning = false;
                    t.expires_at_unix = done_unix + 6;
                    state.toasts.push(std::move(t));
                }
                state.conf_refresh_all_total.store(0, std::memory_order_relaxed);
            }
        });
        recount_pending(state);
    });
}

void refresh_all(app::AppState& state) {
    const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto cooldown = state.settings.confirmations.per_account_cooldown_seconds;
    std::vector<core::Account> snapshots;
    snapshots.reserve(state.vault.accounts.size());
    for (auto& a : state.vault.accounts) {
        if (!a.sda.has_value()) continue;
        if (state.conf_permanent_failure.count(a.id)) continue;
        if (cooldown > 0) {
            const auto it = state.conf_last_refresh_unix.find(a.id);
            if (it != state.conf_last_refresh_unix.end() &&
                now_unix - it->second < cooldown) continue;
        }
        snapshots.push_back(a);
    }
    if (snapshots.empty()) return;

    state.conf_refresh_all_total.store(static_cast<int>(snapshots.size()),
                                        std::memory_order_relaxed);
    state.conf_refresh_all_done.store(0, std::memory_order_relaxed);
    state.conf_refresh_batch_baseline.store(
        state.pending_confirmations_count.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

    const auto stagger_ms = state.settings.confirmations.refresh_stagger_ms;
    app::job_pump::submit([&state, snapshots = std::move(snapshots), stagger_ms]() mutable {
        for (auto& a : snapshots) {
            state.post_ui_callback([&state, a] {
                submit_account_refresh(state, a);
            });
            if (stagger_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(stagger_ms));
            }
        }
    });
}

struct TypeStyle {
    const char* label;
    ImVec4 fill;
};

TypeStyle style_for(sda::ConfirmationType t) {
    switch (t) {
        case sda::ConfirmationType::Trade:             return {"Trade",    theme::accent()};
        case sda::ConfirmationType::MarketListing:     return {"Market",   theme::accent()};
        case sda::ConfirmationType::PhoneNumberChange: return {"Phone",    theme::warning()};
        case sda::ConfirmationType::AccountRecovery:   return {"Recovery", theme::danger()};
        case sda::ConfirmationType::FeatureOptOut:     return {"Opt-out",  theme::warning()};
        case sda::ConfirmationType::Unknown:
        default:                                       return {"Other",    theme::dim_text()};
    }
}

std::string format_relative(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    const auto delta = now_seconds() - unix_seconds;
    if (delta < 0)          return "in the future";
    if (delta < 60)         return std::to_string(delta) + "s ago";
    if (delta < 3600)       return std::to_string(delta / 60) + "m ago";
    if (delta < 86400)      return std::to_string(delta / 3600) + "h ago";
    if (delta < 86400 * 30) return std::to_string(delta / 86400) + "d ago";

    const auto t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

void draw_icon(const sda::Confirmation& c, float size) {
    const float rounding = 6.0F;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(theme::panel_hover());
    dl->AddRectFilled(cursor, ImVec2(cursor.x + size, cursor.y + size), bg, rounding);
    if (c.icon_url.empty()) return;
    if (auto* srv = widgets::avatar_for(c.icon_url)) {
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv),
                            cursor, ImVec2(cursor.x + size, cursor.y + size),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, rounding);
    }
}

enum class CardAction { None, Allow, Deny };

CardAction draw_confirmation_card(const sda::Confirmation& c, float width,
                                   bool* selected) {
    CardAction action = CardAction::None;

    ImGui::PushID(c.id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel());
    ImGui::PushStyleColor(ImGuiCol_Border,
                          (selected && *selected) ? theme::accent() : theme::border());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));

    ImGui::BeginChild("##conf-card", ImVec2(width, kCardHeight),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    const float row_top  = ImGui::GetCursorPosY();
    const float row_left = ImGui::GetCursorPosX();

    if (selected) {
        ImGui::Checkbox("##sel", selected);
        ImGui::SameLine(0.0F, 8.0F);
    }
    draw_icon(c, kIconSize);
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(c.headline.c_str());
    const std::string rel = format_relative(c.creation_unix);
    if (!rel.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextUnformatted(rel.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    {
        const auto style = style_for(c.type);
        const float right_pad = 6.0F;
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowSize().x - kPillWidth - right_pad,
                                   row_top + 2.0F));
        draw_pill(style.label, style.fill, true, kPillWidth);
    }

    ImGui::SetCursorPos(ImVec2(row_left,
                               row_top + kIconSize + ImGui::GetStyle().ItemSpacing.y));

    if (!c.summary.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextWrapped("%s", c.summary.c_str());
        ImGui::PopStyleColor();
    }

    const float spacing   = ImGui::GetStyle().ItemSpacing.x;
    const float content_w = width - 2.0F * ImGui::GetStyle().WindowPadding.x;
    const float btn_w     = (content_w - spacing) * 0.5F;
    const float btn_y     = ImGui::GetWindowSize().y - kButtonRowH;
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, btn_y));

    if (action_button("Allow", ImVec2(btn_w, 0))) action = CardAction::Allow;
    ImGui::SameLine();
    if (action_button("Deny",  ImVec2(btn_w, 0))) action = CardAction::Deny;

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::PopID();

    return action;
}

void submit_single(app::AppState& state, const core::Account& a,
                   const sda::Confirmation& c, bool allow) {
    auto cap_a = a;
    auto cap_c = c;
    app::job_pump::submit([&state, cap_a, cap_c, allow]() mutable {
        std::string err;
        bool ok = sda::respond_to_confirmation(cap_a, cap_c, allow, &err);

        // Cache-staleness recovery: the cid is stable across a fetch, but
        // Steam rotates the nonce when the confirmation is touched elsewhere
        // (e.g., the actual phone app). Refetch once, match by id, retry with
        // the fresh nonce. If the confirmation is gone, treat it as already
        // resolved -- the optimistic erase at the call site stands.
        if (!ok && err == sda::kRespondSuccessFalse) {
            auto fresh = sda::fetch_confirmations(cap_a);
            if (fresh.ok) {
                auto it = std::find_if(fresh.items.begin(), fresh.items.end(),
                                       [&](const sda::Confirmation& f) {
                                           return f.id == cap_c.id;
                                       });
                if (it == fresh.items.end()) return;
                cap_c.nonce = it->nonce;
                err.clear();
                ok = sda::respond_to_confirmation(cap_a, cap_c, allow, &err);
            }
        }

        if (!ok && is_session_error(err)) {
            const auto cd = state.relogin_cooldown_seconds(cap_a.id);
            if (cd > 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "login rate-limited, try again in %lld s",
                              static_cast<long long>(cd));
                err = buf;
            } else {
                std::string relogin_err;
                if (state.auto_relogin(cap_a.id, cap_a, &relogin_err)) {
                    apply_refreshed_tokens(state, cap_a);
                    err.clear();
                    ok = sda::respond_to_confirmation(cap_a, cap_c, allow, &err);
                } else {
                    err = "auto-relogin failed: " + relogin_err;
                }
            }
        }
        if (!ok) {
            std::lock_guard lk(g_mtx);
            g_lists[cap_a.id].last_error = err.empty() ? std::string{"respond failed"} : err;
        }
    });
}

void submit_bulk(app::AppState& state, const core::Account& a,
                 std::vector<sda::Confirmation> items, bool allow) {
    auto cap_a = a;
    auto cap_items = std::move(items);
    app::job_pump::submit([&state, cap_a, cap_items, allow]() mutable {
        std::string err;
        bool ok = sda::respond_bulk(cap_a, cap_items, allow, &err);
        if (!ok && is_session_error(err)) {
            const auto cd = state.relogin_cooldown_seconds(cap_a.id);
            if (cd > 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "login rate-limited, try again in %lld s",
                              static_cast<long long>(cd));
                err = buf;
            } else {
                std::string relogin_err;
                if (state.auto_relogin(cap_a.id, cap_a, &relogin_err)) {
                    apply_refreshed_tokens(state, cap_a);
                    err.clear();
                    ok = sda::respond_bulk(cap_a, cap_items, allow, &err);
                } else {
                    err = "auto-relogin failed: " + relogin_err;
                }
            }
        }
        if (!ok) {
            std::lock_guard lk(g_mtx);
            g_lists[cap_a.id].last_error = err.empty() ? std::string{"bulk respond failed"} : err;
        }
    });
}

// UI-thread-only set of currently-checked confirmation IDs per account.
// Selections are dropped lazily when the underlying confirmation goes away.
std::unordered_map<std::string, std::unordered_set<std::string>> g_selected_conf_ids;

void recount_pending(app::AppState& state) {
    int total = 0;
    {
        std::lock_guard lk(g_mtx);
        for (const auto& [aid, pl] : g_lists) {
            total += static_cast<int>(pl.items.size());
        }
    }
    state.pending_confirmations_count.store(total, std::memory_order_relaxed);
    state.pending_confirmations_loaded.store(true, std::memory_order_relaxed);
}

void erase_item(app::AppState& state, const std::string& account_id,
                const std::string& conf_id) {
    {
        std::lock_guard lk(g_mtx);
        auto& items = g_lists[account_id].items;
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const sda::Confirmation& c) { return c.id == conf_id; }),
                    items.end());
    }
    if (auto it = g_selected_conf_ids.find(account_id); it != g_selected_conf_ids.end()) {
        it->second.erase(conf_id);
    }
    recount_pending(state);
}

void clear_items(app::AppState& state, const std::string& account_id) {
    {
        std::lock_guard lk(g_mtx);
        g_lists[account_id].items.clear();
    }
    g_selected_conf_ids.erase(account_id);
    recount_pending(state);
}

void draw_account_section(app::AppState& state, core::Account& a, const PendingList& snapshot,
                          int type_filter = 0) {
    ImGui::PushID(a.id.c_str());
    ImGui::Indent(kGridInset);

    auto& sel = g_selected_conf_ids[a.id];

    if (state.conf_permanent_failure.count(a.id)) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextUnformatted("Needs attention - repeated session errors. "
                                "Click Refresh to retry once.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    const std::size_t n = snapshot.items.size();
    char approve_label[64];
    char deny_label[64];
    std::snprintf(approve_label, sizeof(approve_label), "Approve all (%zu)", n);
    std::snprintf(deny_label,    sizeof(deny_label),    "Deny all (%zu)",    n);

    auto audit_record = [&](const sda::Confirmation& c, bool allow, sda::AuditSource src) {
        sda::AuditEntry e;
        e.unix_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        e.account_id = a.id;
        e.account_login = a.login;
        e.conf_id = c.id;
        e.conf_type = c.type;
        e.headline = c.headline;
        e.allow = allow;
        e.source = src;
        state.conf_audit.record(std::move(e));
    };

    ImGui::BeginDisabled(snapshot.refreshing || n == 0);
    if (action_button(approve_label, ImVec2(kBulkButtonW, 0))) {
        submit_bulk(state, a, snapshot.items, true);
        for (const auto& c : snapshot.items) audit_record(c, true, sda::AuditSource::UserBulk);
        clear_items(state, a.id);
    }
    ImGui::SameLine();
    if (action_button(deny_label, ImVec2(kBulkButtonW, 0))) {
        submit_bulk(state, a, snapshot.items, false);
        for (const auto& c : snapshot.items) audit_record(c, false, sda::AuditSource::UserBulk);
        clear_items(state, a.id);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    {
        const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto cd_setting = state.settings.confirmations.per_account_cooldown_seconds;
        std::int64_t remaining = 0;
        if (cd_setting > 0) {
            const auto it = state.conf_last_refresh_unix.find(a.id);
            if (it != state.conf_last_refresh_unix.end()) {
                const auto since = now_unix - it->second;
                if (since < cd_setting) remaining = cd_setting - since;
            }
        }
        const bool refresh_disabled = snapshot.refreshing || remaining > 0;
        ImGui::BeginDisabled(refresh_disabled);
        if (action_button("Refresh", ImVec2(kBulkButtonW * 0.6F, 0))) {
            submit_account_refresh(state, a);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (snapshot.refreshing) {
                ImGui::SetTooltip("Refresh in progress...");
            } else if (remaining > 0) {
                ImGui::SetTooltip("Wait %llds before refreshing again.",
                                  static_cast<long long>(remaining));
            }
        }
    }

    std::vector<sda::Confirmation> picked;
    if (!sel.empty()) {
        picked.reserve(sel.size());
        for (const auto& c : snapshot.items) {
            if (sel.count(c.id)) picked.push_back(c);
        }
    }
    if (!picked.empty()) {
        ImGui::SameLine();
        char buf_a[64];
        char buf_d[64];
        std::snprintf(buf_a, sizeof(buf_a), "Approve selected (%zu)", picked.size());
        std::snprintf(buf_d, sizeof(buf_d), "Deny selected (%zu)",    picked.size());
        if (action_button(buf_a, ImVec2(kBulkButtonW, 0))) {
            submit_bulk(state, a, picked, true);
            for (const auto& c : picked) {
                audit_record(c, true, sda::AuditSource::UserBulk);
                erase_item(state, a.id, c.id);
            }
        }
        ImGui::SameLine();
        if (action_button(buf_d, ImVec2(kBulkButtonW, 0))) {
            submit_bulk(state, a, picked, false);
            for (const auto& c : picked) {
                audit_record(c, false, sda::AuditSource::UserBulk);
                erase_item(state, a.id, c.id);
            }
        }
    }

    if (snapshot.refreshing) {
        ImGui::SameLine();
        ImGui::TextDisabled("(refreshing)");
    } else if (!snapshot.last_error.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::Text("(%s)", snapshot.last_error.c_str());
        ImGui::PopStyleColor();

        if (needs_mafile_data(a)) {
            ImGui::SameLine();
            if (action_button("Link maFile...")) {
                link_mafile_for_account(state, a);
            }
        }
    }

    ImGui::Spacing();

    const float left_inset = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x - left_inset;
    const int columns = std::max(1,
        static_cast<int>((avail + kGap) / (kCardMinWidth + kGap)));
    const float card_w = (avail - kGap * static_cast<float>(columns - 1)) /
                         static_cast<float>(columns);

    auto passes_type = [&](sda::ConfirmationType t) {
        switch (type_filter) {
            case 0: return true;
            case 1: return t == sda::ConfirmationType::Trade;
            case 2: return t == sda::ConfirmationType::MarketListing;
            case 3: return t == sda::ConfirmationType::AccountRecovery;
            case 4: return t == sda::ConfirmationType::PhoneNumberChange;
            case 5: return t == sda::ConfirmationType::Unknown ||
                            t == sda::ConfirmationType::FeatureOptOut;
        }
        return true;
    };

    int col = 0;
    for (const auto& c : snapshot.items) {
        if (!passes_type(c.type)) continue;
        if (col > 0) ImGui::SameLine(0.0F, kGap);
        bool checked = sel.count(c.id) != 0;
        const auto act = draw_confirmation_card(c, card_w, &checked);
        if (checked) sel.insert(c.id);
        else         sel.erase(c.id);
        if (act == CardAction::Allow) {
            submit_single(state, a, c, true);
            audit_record(c, true, sda::AuditSource::UserSingle);
            erase_item(state, a.id, c.id);
        } else if (act == CardAction::Deny) {
            submit_single(state, a, c, false);
            audit_record(c, false, sda::AuditSource::UserSingle);
            erase_item(state, a.id, c.id);
        }
        col = (col + 1) % columns;
    }

    ImGui::Unindent(kGridInset);
    ImGui::PopID();
    ImGui::Spacing();
}

void draw_scan_result_modal() {
    if (g_show_scan_modal) {
        ImGui::OpenPopup("Scan maFiles");
        g_show_scan_modal = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    if (!ImGui::BeginPopupModal("Scan maFiles", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ScanResult r;
    {
        std::lock_guard lk(g_mtx);
        r = g_last_scan;
    }

    if (r.targeted == 0) {
        ImGui::TextUnformatted("No accounts are missing maFile data.");
    } else {
        ImGui::Text("Linked %d of %d missing accounts.", r.linked, r.targeted);
        if (r.skipped_encrypted || r.skipped_no_match ||
            r.skipped_ambiguous || r.parse_errors) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::TextUnformatted("Skipped:");
            if (r.skipped_encrypted)
                ImGui::BulletText("encrypted: %d", r.skipped_encrypted);
            if (r.skipped_ambiguous)
                ImGui::BulletText("ambiguous filename: %d", r.skipped_ambiguous);
            if (r.skipped_no_match)
                ImGui::BulletText("content did not match: %d", r.skipped_no_match);
            if (r.parse_errors)
                ImGui::BulletText("parse errors: %d", r.parse_errors);
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    if (action_button("OK")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace

void draw_audit_log_modal(app::AppState& state, bool* p_open) {
    if (!*p_open) return;
    ImGui::OpenPopup("Audit log");
    ImGui::SetNextWindowSize(ImVec2(760.0F, 480.0F), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    if (!ImGui::BeginPopupModal("Audit log", p_open,
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar();
        return;
    }

    static std::string g_audit_search;
    widgets::draw_search_bar(g_audit_search, 320.0F);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu entries)", state.conf_audit.entries().size());

    ImGui::Spacing();
    ImGui::BeginChild("##audit-body", ImVec2(0, -36.0F));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_SizingStretchProp |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##audit", 5, flags)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 110.0F);
        ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthFixed, 140.0F);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableSetupColumn("Headline", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::string search_lower = g_audit_search;
        for (char& c : search_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        const auto& es = state.conf_audit.entries();
        for (auto it = es.rbegin(); it != es.rend(); ++it) {
            const auto& e = *it;
            if (!search_lower.empty()) {
                auto contains = [&](const std::string& s) {
                    std::string lo = s;
                    for (char& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return lo.find(search_lower) != std::string::npos;
                };
                if (!contains(e.account_login) && !contains(e.headline)) continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char tbuf[24];
            std::time_t t = static_cast<std::time_t>(e.unix_time);
            std::tm tm{};
            localtime_s(&tm, &t);
            std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
            ImGui::TextUnformatted(tbuf);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.account_login.c_str());
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                   e.allow ? theme::success() : theme::danger());
            ImGui::TextUnformatted(e.allow ? "allow" : "deny");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            const char* src = "user";
            switch (e.source) {
                case sda::AuditSource::UserSingle: src = "user";   break;
                case sda::AuditSource::UserBulk:   src = "bulk";   break;
                case sda::AuditSource::Auto:       src = "auto";   break;
            }
            ImGui::TextDisabled("%s", src);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", e.headline.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (action_button("Close", ImVec2(100, 0))) {
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

void draw_confirmations(app::AppState& state) {
    static std::string g_conf_search;
    static int g_conf_sort = 0;
    static bool g_audit_open = false;

    ImGui::TextUnformatted("Pending confirmations");
    ImGui::SameLine();
    if (action_button("Refresh all")) {
        refresh_all(state);
    }
    ImGui::SameLine();
    const bool scanning = g_scan_in_progress.load();
    ImGui::BeginDisabled(scanning);
    if (action_button(scanning ? "Scanning..." : "Scan folder for maFiles...")) {
        scan_folder_for_mafiles(state);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (action_button("Audit log")) g_audit_open = true;

    const int total = state.conf_refresh_all_total.load(std::memory_order_relaxed);
    const int done  = state.conf_refresh_all_done.load(std::memory_order_relaxed);
    if (total > 0 && done < total) {
        ImGui::SameLine();
        ImGui::TextDisabled("Refreshing %d/%d", done, total);
    }

    static int g_conf_type_filter = 0;

    ImGui::Spacing();
    if (state.settings.confirmations.show_account_search) {
        widgets::draw_search_bar(g_conf_search, 320.0F);
        ImGui::SameLine();
        ImGui::TextDisabled("Sort");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::Combo("##conf-sort", &g_conf_sort,
                     "Vault order\0Pending desc\0Persona A-Z\0");
        ImGui::PopStyleVar();
        ImGui::SameLine();
        ImGui::TextDisabled("Type");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::Combo("##conf-type", &g_conf_type_filter,
                     "All\0Trade\0Market\0Account recovery\0Phone change\0Other\0");
        ImGui::PopStyleVar();
    }

    auto type_matches_filter = [&](sda::ConfirmationType t) {
        switch (g_conf_type_filter) {
            case 0: return true;
            case 1: return t == sda::ConfirmationType::Trade;
            case 2: return t == sda::ConfirmationType::MarketListing;
            case 3: return t == sda::ConfirmationType::AccountRecovery;
            case 4: return t == sda::ConfirmationType::PhoneNumberChange;
            case 5: return t == sda::ConfirmationType::Unknown ||
                            t == sda::ConfirmationType::FeatureOptOut;
        }
        return true;
    };

    std::vector<std::size_t> indices;
    indices.reserve(state.vault.accounts.size());
    for (std::size_t i = 0; i < state.vault.accounts.size(); ++i) {
        indices.push_back(i);
    }
    if (g_conf_sort == 1) {
        std::stable_sort(indices.begin(), indices.end(),
            [&](std::size_t a, std::size_t b) {
                std::size_t na = 0, nb = 0;
                std::lock_guard lk(g_mtx);
                if (auto it = g_lists.find(state.vault.accounts[a].id); it != g_lists.end())
                    na = it->second.items.size();
                if (auto it = g_lists.find(state.vault.accounts[b].id); it != g_lists.end())
                    nb = it->second.items.size();
                return na > nb;
            });
    } else if (g_conf_sort == 2) {
        std::stable_sort(indices.begin(), indices.end(),
            [&](std::size_t a, std::size_t b) {
                const auto& aa = state.vault.accounts[a];
                const auto& bb = state.vault.accounts[b];
                const std::string la = aa.web.persona_name.empty() ? aa.login : aa.web.persona_name;
                const std::string lb = bb.web.persona_name.empty() ? bb.login : bb.web.persona_name;
                return la < lb;
            });
    }

    const std::string search_lower = [&] {
        std::string s = g_conf_search;
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    auto matches_search = [&](const core::Account& a) {
        if (search_lower.empty()) return true;
        auto contains = [&](const std::string& hay) {
            std::string lower = hay;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lower.find(search_lower) != std::string::npos;
        };
        return contains(a.login) || contains(a.web.persona_name);
    };

    int sections_drawn = 0;
    for (std::size_t idx : indices) {
        auto& a = state.vault.accounts[idx];
        if (!a.sda.has_value()) continue;
        if (!matches_search(a)) continue;

        PendingList snapshot;
        {
            std::lock_guard lk(g_mtx);
            snapshot = g_lists[a.id];
        }
        const bool perm = state.conf_permanent_failure.count(a.id) != 0;
        if (snapshot.items.empty() && !snapshot.refreshing && !perm) {
            if (snapshot.last_error.empty()) continue;
            if (snapshot.last_error.find("no stored password")  != std::string::npos) continue;
            if (snapshot.last_error.find("auto-relogin failed") != std::string::npos) continue;
        }
        if (g_conf_type_filter != 0) {
            bool any_match = false;
            for (const auto& c : snapshot.items) {
                if (type_matches_filter(c.type)) { any_match = true; break; }
            }
            if (!any_match) continue;
        }

        const std::string persona = a.web.persona_name.empty()
                                        ? widgets::login_label(state, a)
                                        : a.web.persona_name;
        ImGui::SeparatorText(persona.c_str());
        draw_account_section(state, a, snapshot, g_conf_type_filter);
        ++sections_drawn;
    }

    if (sections_drawn == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        if (search_lower.empty()) {
            ImGui::TextWrapped("No pending confirmations. Click Refresh all to check Steam.");
        } else {
            ImGui::TextWrapped("No accounts match \"%s\".", g_conf_search.c_str());
        }
        ImGui::PopStyleColor();
    }

    draw_scan_result_modal();
    draw_audit_log_modal(state, &g_audit_open);
}

void confirmations_trigger_refresh_all(app::AppState& state) {
    refresh_all(state);
}

}  // namespace sam::ui::screens
