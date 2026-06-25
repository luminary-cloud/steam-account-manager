#include "app/app_state.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
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

// One account in flight at a time, with a gap between them, plus per-phase ceilings so a
// hung sign-in or pull can't stall the whole sweep. This is the rate limiting.
constexpr auto kInterAccountDelay = std::chrono::seconds(6);
constexpr auto kSigninTimeout = std::chrono::seconds(45);
constexpr auto kPullTimeout = std::chrono::seconds(75);
constexpr std::int64_t kTokenExpiryMargin = 300;  // re-mint a client token this early

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
// (so the GC connect would be rejected) -- i.e. a sign-in is needed before pulling.
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

// Retire the live client (if any), reset per-account state, advance the queue index, and
// arm the inter-account delay.
void finish_current(GcAutoPull& g, steady_clock::time_point now) {
    if (g.client) cs2_gc::retire(std::move(g.client));
    g.phase = GcAutoPull::Phase::Idle;
    g.current_id.clear();
    g.got_data = false;
    g.signin_pending = false;
    g.signin_ok = false;
    ++g.done;
    g.next_start = now + kInterAccountDelay;
}

// Phase B: open a GC session for the current account and stream its data into the cache.
void begin_pull_phase(AppState& st, GcAutoPull& g) {
    core::Account* a = st.find_account(g.current_id);
    if (a == nullptr) {
        finish_current(g, steady_clock::now());
        return;
    }
    g.phase = GcAutoPull::Phase::Pulling;
    g.phase_started = steady_clock::now();
    g.got_data = false;
    g.status = "Pulling " + a->login;
    SAM_LOG_INFO("gc-autopull: pulling '{}'", a->login);

    cs2_gc::Cs2Credentials creds;
    creds.refresh_token = cs2_client_token(*a);
    creds.steam_id = a->steam_id_64;
    creds.proxy = std::string(a->proxy.begin(), a->proxy.end());
    creds.claimed_generation = a->cs2.weekly_drop_claimed_generation;
    creds.claimed_ids = a->cs2.weekly_drop_claimed_ids;
    creds.claimed_names = a->cs2.weekly_drop_claimed_names;

    AppState* stp = &st;
    const std::string aid = g.current_id;
    cs2_gc::Cs2Callbacks cb;
    cb.on_status = [stp, aid](std::string text) {
        stp->post_ui_callback([stp, aid, text = std::move(text)] {
            if (stp->gc_autopull.current_id == aid) stp->gc_autopull.status = std::move(text);
        });
    };
    cb.on_snapshot = [stp, aid](cs2_gc::Snapshot snap) {
        stp->post_ui_callback([stp, aid, snap = std::move(snap)]() mutable {
            stp->apply_gc_snapshot_cache(aid, snap);
            // A valid progress block means the profile (and so the medals) arrived; this
            // account is done. The tick loop retires the client and moves on.
            if (stp->gc_autopull.current_id == aid && snap.progress.valid)
                stp->gc_autopull.got_data = true;
        });
    };
    cb.on_error = [stp, aid](std::string err) {
        stp->post_ui_callback([stp, aid, err = std::move(err)] {
            SAM_LOG_WARN("gc-autopull: '{}' error: {}", aid, err);
            if (stp->gc_autopull.current_id == aid) stp->gc_autopull.got_data = true;
        });
    };
    g.client = std::make_unique<cs2_gc::Cs2GcClient>(std::move(creds), std::move(cb));
}

// Start the next queued account: re-validate the live skips, then either run the automatic
// sign-in (Phase A) or jump straight to the pull (Phase B).
void start_current_account(AppState& st, GcAutoPull& g, steady_clock::time_point now) {
    const std::string aid = g.queue[static_cast<std::size_t>(g.done)];
    core::Account* a = st.find_account(aid);
    if (a == nullptr || a->steam_id_64 == 0 || is_active_steam_user(*a) ||
        is_manually_connected(st, aid)) {
        ++g.done;  // consume the slot; nothing started, so no inter-account delay
        return;
    }

    g.current_id = aid;
    g.phase_started = now;
    if (needs_signin(*a, now_unix())) {
        if (a->password.empty()) {  // no stored password -> can't auto-sign-in
            SAM_LOG_INFO("gc-autopull: skip '{}' (needs manual CS2 sign-in)", a->login);
            g.current_id.clear();
            ++g.done;
            return;
        }
        g.phase = GcAutoPull::Phase::SigningIn;
        g.signin_pending = true;
        g.signin_ok = false;
        g.status = "Signing in " + a->login;
        SAM_LOG_INFO("gc-autopull: signing in '{}'", a->login);
        AppState* stp = &st;
        st.acquire_cm_token(aid, [stp, aid](bool ok, std::string err) {
            if (stp->gc_autopull.current_id != aid) return;  // run moved on / cancelled
            stp->gc_autopull.signin_pending = false;
            stp->gc_autopull.signin_ok = ok;
            if (!ok) SAM_LOG_WARN("gc-autopull: sign-in failed for '{}': {}", aid, err);
        });
        return;
    }
    begin_pull_phase(st, g);
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
    const std::string manual_id =
        (cs2_screen && cs2_screen->client) ? cs2_screen->account_id : std::string{};

    int skipped = 0;
    std::vector<std::string> queue;
    for (const auto& a : vault.accounts) {
        if (a.steam_id_64 == 0) continue;  // no resolved SteamID -> can't reach the GC
        if (is_active_steam_user(a)) { ++skipped; continue; }
        if (!manual_id.empty() && a.id == manual_id) { ++skipped; continue; }
        if (a.cs2.gc_last_pulled_unix != 0 && now - a.cs2.gc_last_pulled_unix < ttl)
            continue;  // cache still fresh
        if (needs_signin(a, now) && a.password.empty()) {
            SAM_LOG_INFO("gc-autopull: skip '{}' (needs manual CS2 sign-in)", a.login);
            ++skipped;
            continue;
        }
        queue.push_back(a.id);
    }

    if (queue.empty()) {
        gc_autopull.status =
            "GC auto-pull: nothing to do (" + std::to_string(skipped) + " skipped)";
        SAM_LOG_INFO("gc-autopull: nothing to pull ({} skipped)", skipped);
        return;
    }

    gc_autopull.active = true;
    gc_autopull.queue = std::move(queue);
    gc_autopull.total = static_cast<int>(gc_autopull.queue.size());
    gc_autopull.skipped = skipped;
    gc_autopull.status = "Starting GC auto-pull";
    gc_autopull.next_start = steady_clock::now();
    SAM_LOG_INFO("gc-autopull: queued {} account(s), {} skipped", gc_autopull.total, skipped);
}

void AppState::tick_gc_autopull() {
    GcAutoPull& g = gc_autopull;
    if (!g.active) return;
    const auto now = steady_clock::now();

    switch (g.phase) {
        case GcAutoPull::Phase::SigningIn:
            if (!g.signin_pending) {
                if (g.signin_ok) begin_pull_phase(*this, g);
                else finish_current(g, now);
            } else if (now - g.phase_started > kSigninTimeout) {
                SAM_LOG_WARN("gc-autopull: sign-in timed out for '{}'", g.current_id);
                finish_current(g, now);
            }
            return;
        case GcAutoPull::Phase::Pulling:
            if (g.got_data) {
                finish_current(g, now);
            } else if (now - g.phase_started > kPullTimeout) {
                SAM_LOG_WARN("gc-autopull: pull timed out for '{}'", g.current_id);
                finish_current(g, now);
            }
            return;
        case GcAutoPull::Phase::Idle:
            break;
    }

    if (now < g.next_start) return;  // inter-account delay
    if (g.done >= g.total) {
        g.active = false;
        g.status = "GC auto-pull done";
        SAM_LOG_INFO("gc-autopull: complete ({} processed, {} pre-skipped)", g.done, g.skipped);
        return;
    }
    start_current_account(*this, g, now);
}

void AppState::cancel_gc_autopull() {
    if (!gc_autopull.active && !gc_autopull.client) return;
    if (gc_autopull.client) cs2_gc::retire(std::move(gc_autopull.client));
    gc_autopull.active = false;
    gc_autopull.phase = GcAutoPull::Phase::Idle;
    gc_autopull.current_id.clear();
    gc_autopull.status = "GC auto-pull stopped";
    SAM_LOG_INFO("gc-autopull: cancelled");
}

}  // namespace sam::app
