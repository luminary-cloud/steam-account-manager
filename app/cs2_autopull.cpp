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

// Accounts eligible to BE the puller, best first: a ready client token (no sign-in) before
// those that must sign in. Never the live Steam account (the puller announces games_played
// and would fight the running client) nor an account already connected on the manual screen
// (avoids a second session for the same account). Those two can still be pull TARGETS.
std::vector<std::string> build_puller_candidates(const AppState& st, std::int64_t now) {
    std::vector<std::string> ready;
    std::vector<std::string> signin;
    for (const auto& a : st.vault.accounts) {
        if (a.steam_id_64 == 0) continue;
        if (is_active_steam_user(a)) continue;
        if (is_manually_connected(st, a.id)) continue;
        if (!needs_signin(a, now)) ready.push_back(a.id);
        else if (!a.password.empty()) signin.push_back(a.id);
    }
    ready.insert(ready.end(), signin.begin(), signin.end());
    return ready;
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

}  // namespace sam::app
