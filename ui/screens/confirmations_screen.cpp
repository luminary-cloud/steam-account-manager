#include "ui/screens/confirmations_screen.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/job_pump.hpp"
#include "core/log.hpp"
#include "core/sda/confirmation.hpp"
#include "ui/screens/confirmations_detail.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/search_bar.hpp"

namespace sam::ui::screens {

using namespace confirmations_detail;

namespace {

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

    // /mobileconf/* returns "Oh nooooooes!" on wrong-audience cookie or bad HMAC;
    // recover with a fresh mobile-audience auto-relogin.
    std::string lower(err);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower.find("oh nooooooes") != std::string::npos) return true;

    // Bare {"success":false} after the caller's stale cid/nonce refetch: likely the
    // access_token drifted off the mobile audience, so auto-relogin.
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

}  // namespace

namespace confirmations_detail {

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

}  // namespace confirmations_detail

namespace {

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

void submit_single(app::AppState& state, const core::Account& a,
                   const sda::Confirmation& c, bool allow) {
    auto cap_a = a;
    auto cap_c = c;
    app::job_pump::submit([&state, cap_a, cap_c, allow]() mutable {
        std::string err;
        bool ok = sda::respond_to_confirmation(cap_a, cap_c, allow, &err);

        // Steam rotates the nonce when the confirmation is touched elsewhere (e.g. the
        // phone app). Refetch once, match by id, retry with the fresh nonce. If it's
        // gone, treat it as already resolved (the optimistic erase at the call site stands).
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

// UI-thread only. Selections are dropped lazily when the confirmation goes away.
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
                set_tooltip("Refresh in progress...");
            } else if (remaining > 0) {
                set_tooltip("Wait %llds before refreshing again.",
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

}  // namespace

void draw_confirmations(app::AppState& state) {
    static std::string g_conf_search;
    static int g_conf_sort = 0;
    static bool g_history_open = false;

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
    if (action_button("History")) g_history_open = true;

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
        separator_text(persona.c_str());
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
    draw_history_modal(state, &g_history_open);
}

void confirmations_trigger_refresh_all(app::AppState& state) {
    refresh_all(state);
}

}  // namespace sam::ui::screens
