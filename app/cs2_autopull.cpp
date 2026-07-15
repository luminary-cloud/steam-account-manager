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

// Per-phase ceilings so a hung sign-in or connect can't stall the sweep. The puller pulls
// every profile over one session, paced ~1.6s apart to stay under the GC's profile-request
// rate limit (see kPaceMs in cs2_gc_client.cpp).
constexpr auto kSigninTimeout = std::chrono::seconds(45);
constexpr auto kConnectTimeout = std::chrono::seconds(90);  // matches GcSession::launch's deadline
constexpr std::int64_t kTokenExpiryMargin = 300;  // re-mint a client token this early
constexpr int kMaxPullerAttempts = 3;  // distinct accounts to try as the puller before giving up

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Unix expiry of the client-audience token cs2_client_token() would use, or 0 if unknown.
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

// True when the account has no usable client token, or its token expires within the margin
// (so the GC connect would be rejected) -- i.e. a sign-in is needed before connecting.
bool needs_signin(const core::Account& a, std::int64_t now) {
    if (cs2_client_token(a).empty()) return true;
    const std::int64_t exp = client_token_expiry(a);
    return exp > 0 && exp <= now + kTokenExpiryMargin;
}

// True when `a` is the account currently signed in to the Steam client on this machine.
bool is_active_steam_user(const core::Account& a) {
    const auto au = platform::registry::read_active_user();
    return au && *au != 0 &&
           static_cast<std::uint32_t>(a.steam_id_64 & 0xffffffffULL) == *au;
}

bool is_manually_connected(const AppState& st, const std::string& aid) {
    return st.cs2_screen && st.cs2_screen->client && st.cs2_screen->account_id == aid;
}

// Forward declarations for the puller state machine (mutually recursive on failure).
void start_puller(AppState& st, GcAutoPull& g, steady_clock::time_point now);
void advance_puller(AppState& st, GcAutoPull& g, steady_clock::time_point now);
void begin_connect(AppState& st, GcAutoPull& g);
void end_autopull(GcAutoPull& g, std::string status);

// Per-account validate sweep (mutually recursive when advancing to the next account).
void begin_validate(AppState& st);
void advance_validate(AppState& st);
void end_validate(AppState& st, std::string status);
constexpr auto kValidateTimeout = std::chrono::seconds(90);
// Gap between per-account validate connections so a large NFA queue never trips Steam's
// CM-logon rate limiter (which would otherwise false-flag good tokens as revoked).
constexpr auto kValidateStagger = std::chrono::seconds(15);

// Accounts eligible to BE the puller, best first. Full-access accounts are preferred:
// they are password-backed and re-mintable, so pulling through them carries none of the
// rotation/rate-limit risk of a shared NFA/cached token. Order: full-access with a ready
// client token -> full-access that can sign in -> NFA/cached with a ready token (last
// resort). Never the live Steam account (the puller announces games_played and would fight
// the running client) nor an account already connected on the manual screen. Those two can
// still be pull TARGETS.
std::vector<std::string> build_puller_candidates(const AppState& st, std::int64_t now) {
    std::vector<std::string> full_ready;   // full-access, client token ready
    std::vector<std::string> full_signin;  // full-access, can sign in with a password
    std::vector<std::string> nfa_ready;    // NFA/cached, token ready (last resort)
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

// Open the GC via the current puller and wire the batch callbacks. All callbacks run on the
// client worker thread, so each marshals onto the UI thread and re-checks gc_autopull.active.
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
            if (it == g.targets.end()) return;  // not a target (or already applied)
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
            g.batch_done = true;  // Pulling ends; Connecting falls back to the next candidate
        });
    };
    g.client = std::make_unique<cs2_gc::Cs2GcClient>(std::move(creds), std::move(cb));
}

// Start the candidate at puller_idx: sign it in first if its client token is missing/stale,
// otherwise connect straight away.
void start_puller(AppState& st, GcAutoPull& g, steady_clock::time_point now) {
    const std::string aid = g.puller_candidates[g.puller_idx];
    core::Account* a = st.find_account(aid);
    if (a == nullptr) {  // vanished; skip to the next candidate
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

// A puller failed (sign-in, connect, or early drop); retire it and try the next candidate,
// ending the sweep once the list is exhausted or the attempt cap is hit.
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
    if (g.client) cs2_gc::retire(std::move(g.client));  // always our own puller, never the manual client
    g.active = false;
    g.phase = GcAutoPull::Phase::Idle;
    g.connected = false;
    g.signin_pending = false;
    g.status = std::move(status);
    SAM_LOG_INFO("gc-autopull: {} ({} of {} applied)", g.status, g.received, g.total);
}

// --- Per-account NFA/cached validate sweep --------------------------------------------

// Connect the current queued account with its own client token: the CM logon validates the
// token (on_logon) and, once the GC is ready, its own profile is pulled (on_profile).
void begin_validate(AppState& st) {
    GcValidate& v = st.gc_validate;
    core::Account* a = st.find_account(v.queue[v.idx]);
    if (a == nullptr) {  // vanished; skip to the next without recording a result
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

// Retire the current account's client and move to the next, ending the sweep when done. The
// next account isn't started here; tick_gc_validate starts it once kValidateStagger passes.
void advance_validate(AppState& st) {
    GcValidate& v = st.gc_validate;
    if (v.client) cs2_gc::retire(std::move(v.client));
    v.connected = false;
    v.pull_issued = false;
    ++v.done;
    ++v.idx;
    if (v.idx >= v.queue.size()) {
        end_validate(st, "NFA validate: done");
        return;
    }
    v.resume_at = steady_clock::now() + kValidateStagger;  // pace the next connection
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

    // Targets: every account with a resolved SteamID whose cache is stale. The live Steam
    // user and the manually-connected account are included (reading a public profile is
    // harmless and refreshes their card for free) -- they just can't be the puller.
    int skipped = 0;
    std::unordered_map<std::uint32_t, std::string> targets;
    for (const auto& a : vault.accounts) {
        if (a.steam_id_64 == 0) continue;  // no resolved SteamID -> can't be addressed
        // NFA/cached accounts get a richer per-account GC connect (own profile + token
        // validation), so they are excluded from the foreign batch here.
        if (a.is_nfa) continue;
        if (a.cs2.gc_last_pulled_unix != 0 && now - a.cs2.gc_last_pulled_unix < ttl) {
            ++skipped;  // cache still fresh
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
            } else if (g.batch_done) {  // an on_error landed before "Ready"
                advance_puller(*this, g, now);
            } else if (now - g.phase_started > kConnectTimeout) {
                SAM_LOG_WARN("gc-autopull: GC connect timed out via '{}'", g.puller_id);
                advance_puller(*this, g, now);
            }
            return;
        case GcAutoPull::Phase::Pulling: {
            // Allow the batch its paced duration (~1.6s/account, the GC rate limit) plus slack
            // before bailing.
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

void AppState::start_gc_validate(bool force) {
    if (gc_validate.active) return;
    if (gc_validate.client) cs2_gc::retire(std::move(gc_validate.client));
    gc_validate = GcValidate{};
    if (!settings.cs2_gc.enabled) return;

    const std::int64_t now = now_unix();
    const int hours = std::clamp(settings.cs2_gc.cache_hours, 1, 24);
    const std::int64_t ttl = static_cast<std::int64_t>(hours) * 3600;

    std::vector<std::string> queue;
    for (const auto& a : vault.accounts) {
        if (!a.is_nfa) continue;  // NFA + cached only (full-access uses the batch autopull)
        if (a.steam_id_64 == 0) continue;
        if (cs2_client_token(a).empty()) continue;   // no client-audience token to connect with
        if (is_active_steam_user(a)) continue;        // don't fight the running Steam client
        if (is_manually_connected(*this, a.id)) continue;
        // Gate on the last *validation* (set on both Valid and Revoked) so a known-revoked
        // token isn't re-checked every sweep, and a never-validated one always runs.
        if (!force && a.nfa_last_validated_unix != 0 &&
            now - a.nfa_last_validated_unix < ttl)
            continue;  // validated recently
        queue.push_back(a.id);
    }
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

void AppState::queue_gc_validate(const std::string& account_id) {
    core::Account* a = find_account(account_id);
    if (a == nullptr || !a->is_nfa || a->steam_id_64 == 0) return;
    if (cs2_client_token(*a).empty()) return;
    if (gc_validate.active) {
        for (const auto& id : gc_validate.queue)
            if (id == account_id) return;  // already queued
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

    // Between accounts (no live client): start the next one once the stagger has elapsed.
    if (!v.client) {
        if (now >= v.resume_at) begin_validate(*this);
        return;
    }

    // Issue the own-profile pull exactly once, after the GC reports Ready.
    if (v.connected && !v.pull_issued && v.client) {
        v.client->pull_profiles({v.current_account_id});
        v.pull_issued = true;
    }

    const bool timed_out = now - v.phase_started > kValidateTimeout;
    if (!v.finished && !timed_out) return;

    // Record the token-validity result for the current account.
    if (auto* a = find_account(v.current_id)) {
        const core::NfaTokenStatus prev = a->nfa_status;
        if (v.logon_seen && v.logon_eresult == 1) {
            a->nfa_status = core::NfaTokenStatus::Valid;
            a->nfa_last_validated_unix = now_unix();
        } else if (v.logon_seen && v.logon_eresult != 0) {
            // Steam actively rejected the logon. The sweep is one-at-a-time + TTL-gated so
            // we never rate-limit ourselves, so a rejection means the token is genuinely bad.
            a->nfa_status = core::NfaTokenStatus::Revoked;
            a->nfa_last_validated_unix = now_unix();
        }
        // else: no CM response (transient) -> leave the prior status untouched.
        if (a->nfa_status != prev) {
            vault_dirty = true;
            save_vault_if_dirty();
            SAM_LOG_INFO("gc-validate: '{}' -> {}", a->login,
                         a->nfa_status == core::NfaTokenStatus::Valid ? "Valid" : "Revoked");
        }
        // Notify only when a token *transitions* into revoked (not every time an already-
        // revoked account is re-checked); clear the flag when it validates again so a later
        // revocation re-notifies. Shares the dead-token de-dupe set.
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

}  // namespace sam::app
