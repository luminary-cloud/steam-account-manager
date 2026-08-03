#include "app/app_state.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/cs2_gc/cs2_gc_client.hpp"
#include "core/log.hpp"
#include "core/steam_login/session.hpp"
#include "platform/registry.hpp"
#include "ui/screens/cs2_screen_state.hpp"
#include "ui/widgets/toast_stack.hpp"

namespace sam::app {

namespace {

using steady_clock = std::chrono::steady_clock;

constexpr auto kSigninTimeout = std::chrono::seconds(45);
constexpr auto kConnectTimeout = std::chrono::seconds(90);
constexpr std::int64_t kTokenExpiryMargin = 300;
constexpr int kMaxPullerAttempts = 3;

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t client_token_expiry(const core::Account& a) {
    if (!a.cm_refresh_token.empty() &&
        steam_login::jwt_audience(a.cm_refresh_token).find("client") != std::string::npos)
        return a.cm_refresh_token_expires > 0 ? a.cm_refresh_token_expires
                                              : steam_login::jwt_expiry(a.cm_refresh_token);
    if (steam_login::jwt_audience(a.refresh_token).find("client") != std::string::npos)
        return a.refresh_token_expires > 0 ? a.refresh_token_expires
                                           : steam_login::jwt_expiry(a.refresh_token);
    return 0;
}

bool needs_signin(const core::Account& a, std::int64_t now) {
    if (cs2_client_token(a).empty()) return true;
    const std::int64_t exp = client_token_expiry(a);
    return exp > 0 && exp <= now + kTokenExpiryMargin;
}

bool is_active_steam_user(const core::Account& a) {
    const auto au = platform::registry::read_active_user();
    return au && *au != 0 &&
           static_cast<std::uint32_t>(a.steam_id_64 & 0xffffffffULL) == *au;
}

bool is_manually_connected(const AppState& st, const std::string& aid) {
    return st.cs2_screen && st.cs2_screen->client && st.cs2_screen->account_id == aid;
}

void start_puller(AppState& st, GcAutoPull& g, steady_clock::time_point now);
void advance_puller(AppState& st, GcAutoPull& g, steady_clock::time_point now);
void begin_connect(AppState& st, GcAutoPull& g);
void end_autopull(GcAutoPull& g, std::string status);

void begin_validate(AppState& st);
void advance_validate(AppState& st);
void end_validate(AppState& st, std::string status);
constexpr auto kValidateTimeout = std::chrono::seconds(90);

constexpr auto kValidateStagger = std::chrono::seconds(15);

std::vector<std::string> build_puller_candidates(const AppState& st, std::int64_t now) {
    std::vector<std::string> full_ready;
    std::vector<std::string> full_signin;
    std::vector<std::string> nfa_ready;
    for (const auto& a : st.vault.accounts) {
        if (a.steam_id_64 == 0) continue;
        if (is_active_steam_user(a)) continue;
        if (is_manually_connected(st, a.id)) continue;
        const bool ready = !needs_signin(a, now);
        if (!a.is_nfa) {
            if (ready) full_ready.push_back(a.id);
            else if (!a.password.empty()) full_signin.push_back(a.id);
        } else if (ready) {
            nfa_ready.push_back(a.id);
        }
    }
    std::vector<std::string> out = std::move(full_ready);
    out.insert(out.end(), full_signin.begin(), full_signin.end());
    out.insert(out.end(), nfa_ready.begin(), nfa_ready.end());
    return out;
}

void begin_connect(AppState& st, GcAutoPull& g) {
    core::Account* a = st.find_account(g.puller_id);
    if (a == nullptr) {
        advance_puller(st, g, steady_clock::now());
        return;
    }
    g.phase = GcAutoPull::Phase::Connecting;
    g.phase_started = steady_clock::now();
    g.connected = false;
    g.batch_done = false;
    g.status = "Connecting to CS2 via " + a->login;
    SAM_LOG_INFO("gc-autopull: connecting GC via puller '{}'", a->login);

    cs2_gc::Cs2Credentials creds;
    creds.refresh_token = cs2_client_token(*a);
    creds.steam_id = a->steam_id_64;
    creds.proxy = std::string(a->proxy.begin(), a->proxy.end());

    AppState* stp = &st;
    cs2_gc::Cs2Callbacks cb;
    cb.on_status = [stp](std::string text) {
        stp->post_ui_callback([stp, text = std::move(text)] {
            GcAutoPull& g = stp->gc_autopull;
            if (!g.active) return;
            g.status = text;
            if (text == "Ready") g.connected = true;
        });
    };
    cb.on_profile = [stp](cs2_gc::ProfilePull pp) {
        stp->post_ui_callback([stp, pp = std::move(pp)]() mutable {
            GcAutoPull& g = stp->gc_autopull;
            if (!g.active) return;
            auto it = g.targets.find(pp.account_id);
            if (it == g.targets.end()) return;
            cs2_gc::Snapshot snap;
            snap.progress = pp.progress;
            snap.medals = std::move(pp.medals);
            snap.featured_medal_defidx = pp.featured_medal_defidx;
            snap.ranks = pp.ranks;
            stp->apply_gc_snapshot_cache(it->second, snap);
            ++g.received;
            g.status = "Pulling GC " + std::to_string(g.received) + "/" + std::to_string(g.total);
        });
    };
    cb.on_profiles_done = [stp](int received, int requested) {
        stp->post_ui_callback([stp, received, requested] {
            GcAutoPull& g = stp->gc_autopull;
            if (!g.active) return;
            g.batch_done = true;
            SAM_LOG_INFO("gc-autopull: batch complete {}/{}", received, requested);
        });
    };
    cb.on_error = [stp](std::string err) {
        stp->post_ui_callback([stp, err = std::move(err)] {
            GcAutoPull& g = stp->gc_autopull;
            if (!g.active) return;
            SAM_LOG_WARN("gc-autopull: puller error: {}", err);
            g.batch_done = true;
        });
    };
    g.client = std::make_unique<cs2_gc::Cs2GcClient>(std::move(creds), std::move(cb));
}

void start_puller(AppState& st, GcAutoPull& g, steady_clock::time_point now) {
    const std::string aid = g.puller_candidates[g.puller_idx];
    core::Account* a = st.find_account(aid);
    if (a == nullptr) {
        advance_puller(st, g, now);
        return;
    }
    g.puller_id = aid;
    g.phase_started = now;
    g.connected = false;
    g.batch_done = false;
    if (needs_signin(*a, now_unix())) {
        g.phase = GcAutoPull::Phase::SigningIn;
        g.signin_pending = true;
        g.signin_ok = false;
        g.status = "Signing in " + a->login;
        SAM_LOG_INFO("gc-autopull: signing in puller '{}'", a->login);
        AppState* stp = &st;
        st.acquire_cm_token(aid, [stp, aid](bool ok, std::string err) {
            if (!stp->gc_autopull.active || stp->gc_autopull.puller_id != aid) return;
            stp->gc_autopull.signin_pending = false;
            stp->gc_autopull.signin_ok = ok;
            if (!ok) SAM_LOG_WARN("gc-autopull: puller sign-in failed for '{}': {}", aid, err);
        });
        return;
    }
    begin_connect(st, g);
}

void advance_puller(AppState& st, GcAutoPull& g, steady_clock::time_point now) {
    if (g.client) cs2_gc::retire(std::move(g.client));
    g.connected = false;
    g.signin_pending = false;
    g.batch_done = false;
    ++g.puller_idx;
    if (g.puller_idx >= g.puller_candidates.size() ||
        g.puller_idx >= static_cast<std::size_t>(kMaxPullerAttempts)) {
        end_autopull(g, "GC auto-pull: could not reach the CS2 GC");
        return;
    }
    start_puller(st, g, now);
}

void end_autopull(GcAutoPull& g, std::string status) {
    if (g.client) cs2_gc::retire(std::move(g.client));
    g.active = false;
    g.phase = GcAutoPull::Phase::Idle;
    g.connected = false;
    g.signin_pending = false;
    g.status = std::move(status);
    SAM_LOG_INFO("gc-autopull: {} ({} of {} applied)", g.status, g.received, g.total);
}

void begin_validate(AppState& st) {
    GcValidate& v = st.gc_validate;
    core::Account* a = st.find_account(v.queue[v.idx]);
    if (a == nullptr) {
        advance_validate(st);
        return;
    }
    v.current_id = a->id;
    v.current_account_id = static_cast<std::uint32_t>(a->steam_id_64 & 0xffffffffULL);
    v.connected = false;
    v.pull_issued = false;
    v.logon_seen = false;
    v.logon_eresult = 0;
    v.profile_applied = false;
    v.finished = false;
    v.phase_started = steady_clock::now();
    v.status = "Validating " + a->login;
    SAM_LOG_INFO("gc-validate: connecting as '{}'", a->login);

    cs2_gc::Cs2Credentials creds;
    creds.refresh_token = cs2_client_token(*a);
    creds.steam_id = a->steam_id_64;
    creds.proxy = std::string(a->proxy.begin(), a->proxy.end());

    AppState* stp = &st;
    cs2_gc::Cs2Callbacks cb;
    cb.on_logon = [stp](int eresult) {
        stp->post_ui_callback([stp, eresult] {
            GcValidate& v = stp->gc_validate;
            if (!v.active) return;
            v.logon_seen = true;
            v.logon_eresult = eresult;
        });
    };
    cb.on_status = [stp](std::string text) {
        stp->post_ui_callback([stp, text = std::move(text)] {
            GcValidate& v = stp->gc_validate;
            if (!v.active) return;
            if (text == "Ready") v.connected = true;
        });
    };
    cb.on_profile = [stp](cs2_gc::ProfilePull pp) {
        stp->post_ui_callback([stp, pp = std::move(pp)]() mutable {
            GcValidate& v = stp->gc_validate;
            if (!v.active || pp.account_id != v.current_account_id) return;
            cs2_gc::Snapshot snap;
            snap.progress = pp.progress;
            snap.medals = std::move(pp.medals);
            snap.featured_medal_defidx = pp.featured_medal_defidx;
            snap.ranks = pp.ranks;
            stp->apply_gc_snapshot_cache(v.current_id, snap);
            v.profile_applied = true;
        });
    };
    cb.on_profiles_done = [stp](int, int) {
        stp->post_ui_callback([stp] {
            GcValidate& v = stp->gc_validate;
            if (v.active) v.finished = true;
        });
    };
    cb.on_error = [stp](std::string err) {
        stp->post_ui_callback([stp, err = std::move(err)] {
            GcValidate& v = stp->gc_validate;
            if (!v.active) return;
            SAM_LOG_WARN("gc-validate: error: {}", err);
            v.finished = true;
        });
    };
    v.client = std::make_unique<cs2_gc::Cs2GcClient>(std::move(creds), std::move(cb));
}

void advance_validate(AppState& st) {
    GcValidate& v = st.gc_validate;
    if (v.client) cs2_gc::retire(std::move(v.client));
    v.connected = false;
    v.pull_issued = false;
    ++v.done;
    ++v.idx;

    if (v.feed_refresh_all) {
        const int done = st.refresh_all_done.fetch_add(1, std::memory_order_relaxed) + 1;
        if (done >= st.refresh_all_total.load(std::memory_order_relaxed)) {
            st.refresh_all_total.store(0, std::memory_order_relaxed);
            st.refresh_all_done.store(0, std::memory_order_relaxed);
        }
    }
    if (v.idx >= v.queue.size()) {
        end_validate(st, "NFA validate: done");
        return;
    }
    v.resume_at = steady_clock::now() + kValidateStagger;
}

void end_validate(AppState& st, std::string status) {
    GcValidate& v = st.gc_validate;
    if (v.client) cs2_gc::retire(std::move(v.client));
    v.active = false;
    v.status = std::move(status);
    SAM_LOG_INFO("gc-validate: {} ({} processed)", v.status, v.done);
}

}  // namespace

std::string cs2_client_token(const core::Account& a) {
    if (!a.cm_refresh_token.empty() &&
        steam_login::jwt_audience(a.cm_refresh_token).find("client") != std::string::npos)
        return std::string(a.cm_refresh_token.begin(), a.cm_refresh_token.end());
    if (steam_login::jwt_audience(a.refresh_token).find("client") != std::string::npos)
        return std::string(a.refresh_token.begin(), a.refresh_token.end());
    return {};
}

void AppState::start_gc_autopull() {
    if (gc_autopull.active) return;
    if (gc_autopull.client) cs2_gc::retire(std::move(gc_autopull.client));
    gc_autopull = GcAutoPull{};

    if (!settings.cs2_gc.enabled) {
        SAM_LOG_INFO("gc-autopull: CS2 GC disabled in settings");
        return;
    }

    const std::int64_t now = now_unix();
    const int hours = std::clamp(settings.cs2_gc.cache_hours, 1, 24);
    const std::int64_t ttl = static_cast<std::int64_t>(hours) * 3600;

    int skipped = 0;
    std::unordered_map<std::uint32_t, std::string> targets;
    for (const auto& a : vault.accounts) {
        if (a.steam_id_64 == 0) continue;

        if (a.is_nfa) continue;
        if (a.cs2.gc_last_pulled_unix != 0 && now - a.cs2.gc_last_pulled_unix < ttl) {
            ++skipped;
            continue;
        }
        targets[static_cast<std::uint32_t>(a.steam_id_64 & 0xffffffffULL)] = a.id;
    }

    if (targets.empty()) {
        gc_autopull.status =
            "GC auto-pull: nothing to do (" + std::to_string(skipped) + " fresh)";
        SAM_LOG_INFO("gc-autopull: nothing to pull ({} fresh)", skipped);
        return;
    }

    std::vector<std::string> pullers = build_puller_candidates(*this, now);
    if (pullers.empty()) {
        gc_autopull.status = "GC auto-pull: no account can reach the CS2 GC";
        SAM_LOG_INFO("gc-autopull: no eligible puller account");
        return;
    }

    gc_autopull.active = true;
    gc_autopull.targets = std::move(targets);
    gc_autopull.total = static_cast<int>(gc_autopull.targets.size());
    gc_autopull.skipped = skipped;
    gc_autopull.puller_candidates = std::move(pullers);
    gc_autopull.status = "Starting GC auto-pull";
    SAM_LOG_INFO("gc-autopull: {} target(s), {} fresh, {} puller candidate(s)",
                 gc_autopull.total, skipped, gc_autopull.puller_candidates.size());
    start_puller(*this, gc_autopull, steady_clock::now());
}

void AppState::tick_gc_autopull() {
    GcAutoPull& g = gc_autopull;
    if (!g.active) return;
    const auto now = steady_clock::now();

    switch (g.phase) {
        case GcAutoPull::Phase::SigningIn:
            if (g.signin_pending) {
                if (now - g.phase_started > kSigninTimeout) {
                    SAM_LOG_WARN("gc-autopull: puller sign-in timed out for '{}'", g.puller_id);
                    advance_puller(*this, g, now);
                }
            } else if (g.signin_ok) {
                begin_connect(*this, g);
            } else {
                advance_puller(*this, g, now);
            }
            return;
        case GcAutoPull::Phase::Connecting:
            if (g.connected) {
                std::vector<std::uint32_t> ids;
                ids.reserve(g.targets.size());
                for (const auto& kv : g.targets) ids.push_back(kv.first);
                if (g.client) g.client->pull_profiles(std::move(ids));
                g.phase = GcAutoPull::Phase::Pulling;
                g.phase_started = now;
                g.status = "Pulling GC 0/" + std::to_string(g.total);
            } else if (g.batch_done) {
                advance_puller(*this, g, now);
            } else if (now - g.phase_started > kConnectTimeout) {
                SAM_LOG_WARN("gc-autopull: GC connect timed out via '{}'", g.puller_id);
                advance_puller(*this, g, now);
            }
            return;
        case GcAutoPull::Phase::Pulling: {

            const auto limit =
                std::chrono::seconds(30) + std::chrono::milliseconds(1600) * g.total;
            if (g.batch_done) {
                end_autopull(g, "GC auto-pull done");
            } else if (now - g.phase_started > limit) {
                SAM_LOG_WARN("gc-autopull: batch timed out");
                end_autopull(g, "GC auto-pull timed out");
            }
            return;
        }
        case GcAutoPull::Phase::Idle:
            return;
    }
}

void AppState::cancel_gc_autopull() {
    if (!gc_autopull.active && !gc_autopull.client) return;
    if (gc_autopull.client) cs2_gc::retire(std::move(gc_autopull.client));
    gc_autopull.active = false;
    gc_autopull.phase = GcAutoPull::Phase::Idle;
    gc_autopull.connected = false;
    gc_autopull.signin_pending = false;
    gc_autopull.status = "GC auto-pull stopped";
    SAM_LOG_INFO("gc-autopull: cancelled");
}

std::vector<std::string> AppState::collect_nfa_validate_ids(bool force) {
    const std::int64_t now = now_unix();
    const int hours = std::clamp(settings.cs2_gc.cache_hours, 1, 24);
    const std::int64_t ttl = static_cast<std::int64_t>(hours) * 3600;

    std::vector<std::string> queue;
    for (const auto& a : vault.accounts) {
        if (!a.is_nfa) continue;
        if (a.steam_id_64 == 0) continue;
        if (cs2_client_token(a).empty()) continue;
        if (is_active_steam_user(a)) continue;
        if (is_manually_connected(*this, a.id)) continue;

        if (!force && a.nfa_last_validated_unix != 0 &&
            now - a.nfa_last_validated_unix < ttl)
            continue;
        queue.push_back(a.id);
    }
    return queue;
}

void AppState::start_gc_validate(bool force) {
    if (gc_validate.active) return;
    if (gc_validate.client) cs2_gc::retire(std::move(gc_validate.client));
    gc_validate = GcValidate{};
    if (!settings.cs2_gc.enabled) return;

    std::vector<std::string> queue = collect_nfa_validate_ids(force);
    if (queue.empty()) {
        gc_validate.status = "NFA validate: nothing to do";
        return;
    }
    gc_validate.active = true;
    gc_validate.queue = std::move(queue);
    gc_validate.status = "Validating NFA/cached accounts";
    SAM_LOG_INFO("gc-validate: {} account(s) queued", gc_validate.queue.size());
    begin_validate(*this);
}

void AppState::start_gc_validate_feed(std::vector<std::string> ids) {
    if (ids.empty()) return;

    if (gc_validate.client) cs2_gc::retire(std::move(gc_validate.client));
    gc_validate = GcValidate{};
    gc_validate.active = true;
    gc_validate.feed_refresh_all = true;
    gc_validate.queue = std::move(ids);
    gc_validate.status = "Refreshing NFA cooldowns";
    SAM_LOG_INFO("gc-validate: {} account(s) queued (refresh-all)", gc_validate.queue.size());
    begin_validate(*this);
}

void AppState::queue_gc_validate(const std::string& account_id) {
    core::Account* a = find_account(account_id);
    if (a == nullptr || a->steam_id_64 == 0) return;

    if (cs2_client_token(*a).empty()) return;
    if (is_active_steam_user(*a) || is_manually_connected(*this, account_id)) return;
    if (gc_validate.active) {
        for (const auto& id : gc_validate.queue)
            if (id == account_id) return;
        gc_validate.queue.push_back(account_id);
        return;
    }
    gc_validate = GcValidate{};
    gc_validate.active = true;
    gc_validate.queue.push_back(account_id);
    gc_validate.status = "Validating account";
    begin_validate(*this);
}

void AppState::tick_gc_validate() {
    GcValidate& v = gc_validate;
    if (!v.active) return;
    const auto now = steady_clock::now();

    if (!v.client) {
        if (now >= v.resume_at) begin_validate(*this);
        return;
    }

    if (v.connected && !v.pull_issued && v.client) {
        v.client->pull_profiles({v.current_account_id});
        v.pull_issued = true;
    }

    const bool timed_out = now - v.phase_started > kValidateTimeout;
    if (!v.finished && !timed_out) return;

    if (auto* a = find_account(v.current_id); a != nullptr && a->is_nfa) {
        const core::NfaTokenStatus prev = a->nfa_status;
        if (v.logon_seen && v.logon_eresult == 1) {
            a->nfa_status = core::NfaTokenStatus::Valid;
            a->nfa_last_validated_unix = now_unix();
        } else if (v.logon_seen && v.logon_eresult != 0) {

            a->nfa_status = core::NfaTokenStatus::Revoked;
            a->nfa_last_validated_unix = now_unix();
        }

        if (a->nfa_status != prev) {
            vault_dirty = true;
            save_vault_if_dirty();
            SAM_LOG_INFO("gc-validate: '{}' -> {}", a->login,
                         a->nfa_status == core::NfaTokenStatus::Valid ? "Valid" : "Revoked");
        }

        if (a->nfa_status == core::NfaTokenStatus::Valid) {
            nfa_dead_notified.erase(a->id);
        } else if (a->nfa_status == core::NfaTokenStatus::Revoked &&
                   prev != core::NfaTokenStatus::Revoked &&
                   nfa_dead_notified.insert(a->id).second) {
            ui::widgets::ToastItem t;
            t.id = "nfa-revoked-" + a->id;
            t.message = "NFA token revoked - " +
                        (a->web.persona_name.empty() ? a->login : a->web.persona_name) +
                        " (re-import a token)";
            t.account_id = a->id;
            t.is_warning = true;
            t.expires_at_unix = now_unix() + settings.notifications.toast_duration_seconds;
            toasts.push(std::move(t));
        }
    }
    if (timed_out && !v.finished)
        SAM_LOG_WARN("gc-validate: '{}' timed out", v.current_id);

    advance_validate(*this);
}

void AppState::cancel_gc_validate() {
    if (!gc_validate.active && !gc_validate.client) return;
    if (gc_validate.client) cs2_gc::retire(std::move(gc_validate.client));
    gc_validate.active = false;
    gc_validate.status = "NFA validate stopped";
    SAM_LOG_INFO("gc-validate: cancelled");
}

void AppState::refresh_gc_all(bool announce) {
    if (!settings.cs2_gc.enabled) return;

    start_gc_autopull();
    start_gc_validate(false);

    if (announce && !gc_autopull.active && !gc_validate.active) {
        ui::widgets::ToastItem t;
        t.id = "refresh-gc-uptodate";
        t.message = "GC data is up to date for all accounts";
        t.is_warning = true;
        t.expires_at_unix = now_unix() + settings.notifications.toast_duration_seconds;
        toasts.push(std::move(t));
    }
}

}  // namespace sam::app
