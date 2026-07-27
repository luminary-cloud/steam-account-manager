#include "app/app_state.hpp"
#include "app/app_state_internal.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "app/job_pump.hpp"
#include "core/account_store/ban_diff.hpp"
#include "core/crypto/rng.hpp"
#include "core/http/client.hpp"
#include "core/launch/first_login_reapply.hpp"
#include "core/log.hpp"
#include "core/steam_api/ban_check.hpp"
#include "core/steam_api/summaries.hpp"
#include "core/steam_gcpd/gcpd_parser.hpp"
#include "core/steam_gcpd/gcpd_scraper.hpp"
#include "core/steam_spend/spend_parser.hpp"
#include "core/steam_spend/spend_scraper.hpp"
#include "core/steam_local/connect_cache.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/steam_login/mobile_auth.hpp"
#include "core/steam_login/session.hpp"
#include "core/steam_login/web_session.hpp"
#include "core/trade/trade_link_fetch.hpp"
#include "platform/clipboard.hpp"
#include "platform/registry.hpp"
#include "platform/tray_icon.hpp"

namespace sam::app {

namespace {
// Diffs the about-to-be-written ban/cs2 state, persists every enabled event, and
// updates a.prev_snapshot. Returns the persisted events for the caller to surface.
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
            if (!detail::event_enabled(state.settings.notifications, ev.kind)) continue;
            persisted.push_back(ev);
            state.notifications.push(std::move(ev));
        }
    }
    a.prev_snapshot = core::snapshot_from(new_bans, new_cs2);
    a.prev_snapshot.snapshot_unix = now;
    return persisted;
}
}  // namespace

std::int64_t AppState::refresh_cooldown_seconds(const std::string& id) const {
    auto it = last_refresh_unix.find(id);
    if (it == last_refresh_unix.end()) return 0;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto elapsed = now - it->second;
    if (elapsed >= detail::kMinAccountRefreshSeconds) return 0;
    return detail::kMinAccountRefreshSeconds - elapsed;
}

std::int64_t AppState::persona_change_cooldown_seconds(const std::string& id) const {
    auto it = last_persona_change_unix.find(id);
    if (it == last_persona_change_unix.end()) return 0;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto elapsed = now - it->second;
    if (elapsed >= detail::kPersonaChangeCooldownSeconds) return 0;
    return detail::kPersonaChangeCooldownSeconds - elapsed;
}

void AppState::refresh_account_data(bool force, bool announce) {
    if (settings.web_api_key.empty()) {
        SAM_LOG_WARN("refresh_all: no web API key");
        refresh_web_phase_done.store(true, std::memory_order_relaxed);
        return;
    }

    // Don't overlap an in-flight sweep: the per-account phase (GCPD scrape + NFA GC logins) keeps
    // refresh_all_total > 0 while it runs, so a second trigger is ignored until it finishes.
    // Otherwise rapid triggers are harmless -- cache-gating makes a repeat a no-op and
    // refresh_single_account keeps its own per-account cooldowns.
    if (refresh_all_total.load(std::memory_order_relaxed) > 0) {
        SAM_LOG_INFO("refresh_all: a refresh is already in progress");
        return;
    }

    // Resolve missing steam_ids from loginusers.vdf so manually/maFile-added
    // accounts can still be refreshed.
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

    // Decide the work up front (before the bulk call updates any timestamps), so each data
    // source is gated on its OWN freshness: Steam Web API on web.last_refreshed_unix, the GCPD
    // scrape on cs2.last_refreshed_unix, the NFA GC pull on nfa_last_validated_unix.
    const auto batch_now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::int64_t steam_ttl = static_cast<std::int64_t>(settings.steam_cache_hours) * 3600;

    // Phase 1: bulk Steam Web API for accounts whose public data is stale.
    std::vector<std::uint64_t> ids;
    for (const auto& a : vault.accounts) {
        if (a.steam_id_64 == 0) continue;
        if (!force && a.web.last_refreshed_unix != 0 &&
            batch_now - a.web.last_refreshed_unix < steam_ttl)
            continue;
        ids.push_back(a.steam_id_64);
    }

    // Phase 2a: per-account GCPD scrape for full-access accounts with a usable web session
    // whose GCPD data is stale. (NFA/cached can't scrape GCPD -- they use the GC below.) Gate
    // on the GCPD-specific timestamp, not cs2.last_refreshed_unix, since a GC pull also stamps
    // the latter and would otherwise suppress the cooldown scrape.
    std::vector<std::string> gcpd_ids;
    if (settings.gcpd_enabled) {
        for (const auto& a : vault.accounts) {
            if (a.is_nfa) continue;
            if (a.steam_id_64 == 0) continue;
            if (a.session_id.empty() || a.steam_login_secure.empty()) continue;
            if (!force) {
                auto it = last_gcpd_refresh_unix.find(a.id);
                if (it != last_gcpd_refresh_unix.end() && batch_now - it->second < steam_ttl)
                    continue;
            }
            gcpd_ids.push_back(a.id);
        }
    }

    // Phase 2b: NFA/cached own-session GC login (competitive cooldown + ranks). Skipped in the
    // headless --startup run, which never ticks the per-frame GC machine that would advance it.
    std::vector<std::string> nfa_ids;
    if (settings.cs2_gc.enabled && !headless) {
        nfa_ids = collect_nfa_validate_ids(force);
    }

    if (ids.empty() && gcpd_ids.empty() && nfa_ids.empty()) {
        SAM_LOG_INFO("refresh_all: everything within the cache window");
        if (announce) toasts.push_summary("All accounts are up to date");
        refresh_web_phase_done.store(true, std::memory_order_relaxed);
        refresh_all_total.store(0, std::memory_order_relaxed);
        return;
    }

    SAM_LOG_INFO("refresh_all: steam={} gcpd={} nfa={} accounts", ids.size(), gcpd_ids.size(),
                 nfa_ids.size());

    // Arm the single per-account counter spanning the GCPD scrape and the NFA GC logins. The
    // bulk Steam call has no per-account granularity, so it isn't counted. Every path that ends
    // the batch must land total back at 0 (startup_refresh_complete() depends on it).
    refresh_all_total.store(static_cast<int>(gcpd_ids.size() + nfa_ids.size()),
                            std::memory_order_relaxed);
    refresh_all_done.store(0, std::memory_order_relaxed);
    session_event_count = 0;
    session_event_message.clear();

    // Kick the NFA/cached GC sweep now -- it's independent of the Steam Web API and feeds the
    // shared counter as each account finishes.
    if (!nfa_ids.empty()) start_gc_validate_feed(std::move(nfa_ids));

    if (ids.empty()) {
        // No stale Steam data; go straight to the GCPD scrape (NFA already running).
        refresh_web_phase_done.store(true, std::memory_order_relaxed);
        post_gcpd_sweep(std::move(gcpd_ids));
        return;
    }

    refresh_web_phase_done.store(false, std::memory_order_relaxed);
    steam_api::WebApiConfig cfg;
    cfg.api_key = settings.web_api_key;

    job_pump::submit([cfg, ids, gcpd_ids, this] {
        auto bans = steam_api::fetch_bans(cfg, ids);
        auto summaries = steam_api::fetch_summaries(cfg, ids);

        std::lock_guard lk(job_mutex);
        completed_jobs.push_back({"", [this, gcpd_ids,
                                       bans = std::move(bans),
                                       summaries = std::move(summaries)] {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            int accounts_with_events = 0;
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
                        per_account_events.emplace_back(a.id, std::move(evs));
                    }
                }
            }
            vault_dirty = true;
            save_vault_if_dirty();

            // Coalesce across accounts past coalesce_threshold; else per-account.
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
                            detail::push_toasts_for(*this, *a, evs, now);
                        }
                    }
                }
            }

            // Accumulate web events for one balloon; flushed after the last GCPD account so it
            // covers cooldown changes (or immediately if no GCPD pass runs).
            for (const auto& [acc_id, evs] : per_account_events) {
                if (auto* a = find_account(acc_id)) detail::note_session_event(*this, *a, evs);
            }
            refresh_web_phase_done.store(true, std::memory_order_relaxed);

            if (gcpd_ids.empty()) {
                // No GCPD phase. If the NFA sweep isn't carrying the counter either, close it out.
                if (refresh_all_total.load(std::memory_order_relaxed) == 0)
                    detail::flush_native_notification(*this);
                return;
            }
            post_gcpd_sweep(gcpd_ids);
        }});
    });
}

void AppState::post_gcpd_sweep(std::vector<std::string> ids) {
    if (ids.empty()) return;
    SAM_LOG_INFO("refresh_all: staggering GCPD scrape for {} accounts", ids.size());
    job_pump::submit([this, ids = std::move(ids)] {
        for (const auto& aid : ids) {
            {
                std::lock_guard inner_lk(job_mutex);
                completed_jobs.push_back({"", [this, aid] {
                    refresh_single_account(aid, /*batch_refresh=*/true);
                }});
            }
            if (!job_pump::interruptible_sleep(std::chrono::seconds(2))) break;
        }
    });
}

void AppState::refresh_accounts_staggered(std::vector<std::string> ids,
                                          std::chrono::seconds stagger, bool allow_gcpd) {
    if (ids.empty()) return;
    if (settings.web_api_key.empty()) {
        SAM_LOG_WARN("refresh_staggered: no web API key, skipping {} accounts",
                     ids.size());
        return;
    }
    SAM_LOG_INFO("refresh_staggered: queuing {} accounts ({}s apart)",
                 ids.size(), stagger.count());
    job_pump::submit([this, ids = std::move(ids), stagger, allow_gcpd] {
        for (const auto& aid : ids) {
            {
                std::lock_guard inner_lk(job_mutex);
                completed_jobs.push_back({"", [this, aid, allow_gcpd] {
                    refresh_single_account(aid, /*batch_refresh=*/true, allow_gcpd);
                }});
            }
            if (!job_pump::interruptible_sleep(stagger)) break;
        }
    });
}

void AppState::refresh_single_account(const std::string& id, bool batch_refresh,
                                      bool allow_gcpd) {
    auto* acc = find_account(id);
    if (!acc) { SAM_LOG_WARN("refresh: account '{}' not found", id); return; }
    if (settings.web_api_key.empty()) { SAM_LOG_WARN("refresh: no web API key configured"); return; }
    if (refreshing_ids.count(id)) { return; }

    // Per-account rate limit against Steam's per-IP throttle. auto-relogin has
    // its own 5-minute cooldown.
    {
        auto it = last_refresh_unix.find(id);
        if (it != last_refresh_unix.end()) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now - it->second < detail::kMinAccountRefreshSeconds) {
                SAM_LOG_INFO("refresh: rate-limited for '{}' ({}s since last)",
                             acc->login, now - it->second);
                return;
            }
        }
    }

    SAM_LOG_INFO("refresh: starting for '{}' (steam_id={})", acc->login, acc->steam_id_64);

    // Resolve a missing steam_id from loginusers.vdf before dispatching a worker.
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

    // Thin Account copy for the worker: the vault lives on the UI thread, so
    // never hold a pointer into it.
    core::Account creds;
    creds.steam_id_64 = acc->steam_id_64;
    creds.login = acc->login;
    creds.session_id = acc->session_id;
    creds.steam_login_secure = acc->steam_login_secure;
    creds.refresh_token = acc->refresh_token;
    creds.access_token = acc->access_token;
    creds.access_token_expires = acc->access_token_expires;
    creds.proxy = acc->proxy;

    // Silent auto-relogin needs the password and (for SDA accounts) the
    // shared_secret; default_guard_provider does nothing without a TOTP source.
    crypto::SecureString relogin_password = acc->password;
    std::optional<core::SteamGuardAccount> relogin_sda = acc->sda;
    const bool is_nfa = acc->is_nfa;

    bool gcpd_enabled = settings.gcpd_enabled && allow_gcpd;
    if (gcpd_enabled) {
        // GCPD scrape is heavy; separate per-account cooldown so launch-wide
        // refreshes don't crawl the gcpd page every time.
        auto it = last_gcpd_refresh_unix.find(id);
        if (it != last_gcpd_refresh_unix.end()) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now - it->second < detail::kMinGcpdRefreshSeconds) {
                SAM_LOG_INFO("refresh: gcpd rate-limited for '{}' ({}s since last)",
                             acc->login, now - it->second);
                gcpd_enabled = false;
            }
        }
    }

    job_pump::submit([cfg, resolved, aid, creds, relogin_password, relogin_sda,
                      gcpd_enabled, batch_refresh, is_nfa, this]() mutable {
        http::ScopedProxy proxy_guard(std::string(creds.proxy.data(), creds.proxy.size()));
        SAM_LOG_INFO("refresh: fetching bans/summaries/level/games for {}", resolved);
        std::vector<std::uint64_t> ids{resolved};
        auto bans = steam_api::fetch_bans(cfg, ids);
        auto summaries = steam_api::fetch_summaries(cfg, ids);
        auto level = steam_api::fetch_steam_level(cfg, resolved);
        auto games = steam_api::fetch_owned_games(cfg, resolved);

        // Web-session maintenance for the GCPD scraper:
        //  1. refresh_token + stale access_token: mint a fresh community-scoped
        //     access_token via GenerateAccessTokenForApp.
        //  2. on failure: silent auto-relogin with stored password + shared_secret.
        //  3. transfer_login (finalizelogin + settoken) for a fresh
        //     steamLoginSecure cookie (these expire ~24h even with a valid token).
        //  4. all failed and no cookie left: queue a UI re-login prompt.
        bool token_refreshed = false;
        bool need_relogin = false;
        bool nfa_token_dead = false;
        std::optional<steam_gcpd::MatchmakingData> mm_parsed;
        std::optional<steam_gcpd::AccountMainData> am_parsed;

        // NFA = client-scoped refresh token, which Steam won't exchange for a web
        // session over HTTP (AccessDenied). Skip token refresh + GCPD; read
        // liveness from the JWT itself.
        if (is_nfa) {
            const std::int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            nfa_token_dead = creds.refresh_token.empty() ||
                steam_login::jwt_expiry(creds.refresh_token) <= now_s ||
                steam_login::jwt_audience(creds.refresh_token).find("client")
                    == std::string::npos;
        }

        if (!is_nfa) {
        // True iff we now hold a fresh refresh_token + access_token. Rate-limited
        // to one attempt / 5 min / account so a bad password can't pin Steam's
        // IP rate-limiter.
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
                    // No UI prompt from a worker thread; only the shared_secret
                    // TOTP path can succeed silently.
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
            // run_full_login already registered the community session; mint_cookie
            // is a no-op here but load-bearing for the steady-state 24h refresh.
            creds.steam_login_secure   = std::move(result.account.steam_login_secure);
            if (!result.account.session_id.empty()) {
                creds.session_id = std::move(result.account.session_id);
            } else if (creds.session_id.empty()) {
                creds.session_id = crypto::random_session_id();
            }
            return true;
        };

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

        // Mint a fresh community-domain steamLoginSecure cookie. transfer_login
        // failure usually means a wrong-audience refresh_token; one auto-relogin
        // below gets a token whose `aud` includes web:community.
        auto mint_cookie = [&]() -> bool {
            if (creds.refresh_token.empty() || creds.session_id.empty()) return false;
            std::string new_cookie;
            // finalizelogin takes the refresh_token (not the access_token) as nonce.
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

        // GCPD HTML scraper: premier_rating, wingman_rank, cooldown_* from
        // matchmaking; CS2 level + XP from accountmain (prime_status inferred).
        const bool gcpd_creds_ok = !creds.session_id.empty() && !creds.steam_login_secure.empty();
        if (gcpd_enabled && gcpd_creds_ok) {
            // Session-expired detection is content-based: Steam 302-rewrites
            // /profiles/{id}/gcpd/730 to /id/{vanity}/gcpd/730 for vanity accounts,
            // so status alone doesn't tell us. Check the body shape.
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

        // Snapshot refreshed-token values for the UI thread to write to the vault.
        crypto::SecureString refreshed_at, refreshed_rt, refreshed_ls;
        std::string refreshed_sid;
        std::int64_t refreshed_exp = 0;
        if (token_refreshed) {
            refreshed_at = creds.access_token;
            refreshed_rt = creds.refresh_token;       // GenerateAccessTokenForApp can rotate this
            refreshed_ls = creds.steam_login_secure;
            // Persist any auto-relogin-minted session_id so the next refresh hits
            // mint_cookie() instead of the 5-minute auto-relogin cooldown.
            refreshed_sid = creds.session_id;
            refreshed_exp = creds.access_token_expires;
        }
        std::string relogin_login;
        if (need_relogin) {
            // Batch refresh can't silently re-login a non-SDA account (no
            // shared_secret, no worker-thread prompt), so suppress the popup; don't
            // shove a Full Login wizard at launch. Manual refresh is unchanged.
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
                // Prime inference: non-Prime CS2 accounts can't earn XP, so
                // level > 1 (or level 1 with any XP) means Prime. The primeaccount
                // GCPD tab is no longer reliable for this in CS2.
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
                        detail::note_session_event(*this, *a, evs);
                    } else {
                        detail::push_toasts_for(*this, *a, evs, now_unix);
                        detail::push_native_notification(*this, detail::toast_message_for(*a, evs.front()),
                                                 detail::ban_event_is_cooldown(evs.front().kind));
                    }
                }
            }
            if (batch_refresh) {
                const int done = refresh_all_done.fetch_add(1, std::memory_order_relaxed) + 1;
                if (done >= refresh_all_total.load(std::memory_order_relaxed)) {
                    // Batch over: clear the denominator so the counter hides and the headless
                    // --startup gate reads "complete" rather than 0-of-a-stale-total.
                    refresh_all_total.store(0, std::memory_order_relaxed);
                    refresh_all_done.store(0, std::memory_order_relaxed);
                    detail::flush_native_notification(*this);
                }
            }

            if (!relogin_login.empty() && !pending_relogin_login.has_value()) {
                SAM_LOG_INFO("refresh: queuing UI re-login prompt for '{}'", relogin_login);
                pending_relogin_login = relogin_login;
            }

            // Notify once per dead NFA token. Detection is from the JWT, not an
            // API result, so it can't false-positive on the expected AccessDenied.
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
                detail::push_native_notification(*this, msg, true);
            }

            SAM_LOG_INFO("refresh: done for '{}', bans={} summaries={} cs2_touched={}",
                         aid, bans.size(), summaries.size(), cs2_touched);
            vault_dirty = true;
            save_vault_if_dirty();
        }});
    });
}

void AppState::pull_all_for_account(const std::string& id) {
    core::Account* a = find_account(id);
    if (a == nullptr) return;
    const bool nfa = a->is_nfa;  // covers cached (cached = NFA + a tag)
    // Steam Web API for all; refresh_single_account also scrapes GCPD for full-access.
    refresh_single_account(id);
    if (nfa) {
        // NFA/cached: no GCPD/spend (no web session / no password). Validate the token and
        // pull the account's own CS2 profile over the GC.
        queue_gc_validate(id);
    } else {
        // Full-access: balance + GC medals/level (the freshly added account is a stale
        // target, so the batch autopull picks it up).
        refresh_spend(id, /*quiet=*/true);
        start_gc_autopull();
    }
}

void AppState::auto_refresh_all() {
    if (!unlocked) return;
    // Mirror the manual buttons, cache-gated and silent (no "up to date" toast on a heartbeat
    // where nothing is stale): Refresh all (Steam + GCPD + NFA GC cooldown) and Refresh GC
    // (batch full-access + NFA own-session). Each self-skips fresh accounts via its own TTL.
    refresh_account_data(/*force=*/false, /*announce=*/false);
    refresh_gc_all(/*announce=*/false);
}

void AppState::refresh_spend(const std::string& id, bool quiet) {
    auto* acc = find_account(id);
    if (!acc) return;

    auto toast = [this, quiet](const std::string& aid, std::string msg, bool warn) {
        if (quiet) return;
        ui::widgets::ToastItem t;
        t.id = "spend-" + aid;
        t.message = std::move(msg);
        t.account_id = aid;
        t.is_warning = warn;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        t.expires_at_unix = now + settings.notifications.toast_duration_seconds;
        toasts.push(std::move(t));
    };

    if (acc->is_nfa) {
        toast(id, "Spend: not available for token-only (NFA) accounts", true);
        return;
    }
    if (acc->password.empty()) {
        toast(id, "Spend: needs a stored password to sign in", true);
        return;
    }
    if (acc->steam_id_64 == 0) {
        toast(id, "Spend: refresh the account first (no SteamID yet)", true);
        return;
    }
    if (spend_fetching_ids.count(id)) return;
    spend_fetching_ids.insert(id);

    // Credentials snapshot for the worker; the vault lives on the UI thread.
    core::Account creds;
    creds.login                = acc->login;
    creds.password             = acc->password;
    creds.sda                  = acc->sda;
    creds.steam_id_64          = acc->steam_id_64;
    creds.session_id           = acc->session_id;
    creds.refresh_token        = acc->refresh_token;
    creds.access_token         = acc->access_token;
    creds.access_token_expires = acc->access_token_expires;
    creds.proxy                = acc->proxy;

    const std::string aid = id;
    const std::string login_name = acc->login;
    SAM_LOG_INFO("spend: refresh requested for '{}'", login_name);

    job_pump::submit([this, aid, login_name, creds, toast]() mutable {
        http::ScopedProxy proxy_guard(std::string(creds.proxy.data(), creds.proxy.size()));
        // AccountSpend needs a freshly-password-authed web:help session (step-up
        // auth). The full login gives a fresh-oat session; the scraper then lets
        // help.steampowered.com bootstrap its own cookie across the redirects.
        std::string err;
        if (!auto_relogin(aid, creds, &err)) {
            SAM_LOG_WARN("spend: sign-in failed for '{}': {}", login_name, err);
            post_ui_callback([this, aid, err, toast]() {
                spend_fetching_ids.erase(aid);
                tally_spend_progress();
                toast(aid, "Spend: sign-in failed (" +
                          (err.empty() ? std::string("unknown") : err) + ")", true);
            });
            return;
        }

        const auto resp = steam_spend::fetch_account_spend(creds);
        std::optional<steam_spend::SpendData> parsed;
        std::string fail_msg;
        if (resp.status == 200 && steam_spend::looks_like_spend_page(resp.body)) {
            parsed = steam_spend::parse_account_spend(resp.body);
        } else if (resp.status == 200 && steam_spend::looks_like_login_page(resp.body)) {
            fail_msg = "Spend: Steam still requires a manual sign-in for this page";
        } else if (resp.transport_error) {
            fail_msg = "Spend: request failed (" + resp.error_message + ")";
        } else {
            fail_msg = "Spend: unexpected response (status " + std::to_string(resp.status) + ")";
        }

        // Carry the fresh session back to persist it.
        crypto::SecureString rt = creds.refresh_token;
        crypto::SecureString at = creds.access_token;
        crypto::SecureString ls = creds.steam_login_secure;
        std::string sid = creds.session_id;
        const std::int64_t exp = creds.access_token_expires;

        post_ui_callback([this, aid, login_name, parsed, fail_msg, toast,
                          rt = std::move(rt), at = std::move(at), ls = std::move(ls),
                          sid = std::move(sid), exp]() mutable {
            spend_fetching_ids.erase(aid);
            tally_spend_progress();
            auto* a = find_account(aid);
            if (!a) return;
            if (!rt.empty()) a->refresh_token = std::move(rt);
            if (!at.empty()) { a->access_token = std::move(at); a->access_token_expires = exp; }
            if (!ls.empty()) a->steam_login_secure = std::move(ls);
            if (!sid.empty()) a->session_id = std::move(sid);

            if (parsed && parsed->found_total && parsed->total_spend_cents >= 0) {
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                a->funds.total_spend_usd_cents = parsed->total_spend_cents;
                a->funds.currency_is_usd       = parsed->currency_is_usd;
                a->funds.currency              = parsed->currency;
                a->funds.last_refreshed_unix   = now;
                std::string amt;
                if (parsed->currency_is_usd) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "$%lld.%02d",
                                  static_cast<long long>(parsed->total_spend_cents / 100),
                                  static_cast<int>(parsed->total_spend_cents % 100));
                    amt = buf;
                } else {
                    amt = std::to_string(parsed->total_spend_cents / 100) + " " + parsed->currency;
                }
                SAM_LOG_INFO("spend: '{}' total spend {} ({} cents)",
                             login_name, amt, parsed->total_spend_cents);
                toast(aid, login_name + ": spent " + amt, false);
            } else {
                toast(aid, fail_msg.empty() ? "Spend: no data found" : fail_msg, true);
            }
            vault_dirty = true;
            save_vault_if_dirty();
        });
    });
}

void AppState::copy_trade_link(const std::string& id) {
    auto* acc = find_account(id);
    if (!acc) return;

    constexpr std::int64_t kTradeLinkCacheSeconds = 7 * 24 * 3600;  // re-fetch weekly
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto toast = [this](const std::string& aid, std::string msg, bool warn) {
        ui::widgets::ToastItem t;
        t.id = "tradelink-" + aid;
        t.message = std::move(msg);
        t.account_id = aid;
        t.is_warning = warn;
        const auto n = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        t.expires_at_unix = n + settings.notifications.toast_duration_seconds;
        toasts.push(std::move(t));
    };

    const std::string login_name = acc->login;
    const bool cached = acc->trade_url.has_value() && !acc->trade_url->empty();
    const std::string cached_url = cached ? *acc->trade_url : std::string{};
    const bool fresh = cached &&
        (now - acc->trade_url_fetched_unix) < kTradeLinkCacheSeconds;

    // Fresh cache: copy straight away, no network.
    if (fresh) {
        platform::clipboard::set_text(cached_url);
        toast(id, login_name + ": trade link copied", false);
        return;
    }

    // NFA accounts can't mint a web session to scrape, so the best we can do is
    // hand back a previously-cached link (even if stale).
    if (acc->is_nfa) {
        if (cached) {
            platform::clipboard::set_text(cached_url);
            toast(id, login_name + ": trade link copied (cached)", false);
        } else {
            toast(id, "Trade link: not available for token-only (NFA) accounts", true);
        }
        return;
    }
    if (acc->steam_id_64 == 0) {
        toast(id, "Trade link: refresh the account first (no SteamID yet)", true);
        return;
    }
    if (trade_link_fetching_ids.count(id)) return;
    trade_link_fetching_ids.insert(id);

    // Credentials snapshot for the worker; the vault lives on the UI thread.
    core::Account creds;
    creds.login                = acc->login;
    creds.password             = acc->password;
    creds.sda                  = acc->sda;
    creds.steam_id_64          = acc->steam_id_64;
    creds.session_id           = acc->session_id;
    creds.refresh_token        = acc->refresh_token;
    creds.access_token         = acc->access_token;
    creds.access_token_expires = acc->access_token_expires;
    creds.steam_login_secure   = acc->steam_login_secure;
    creds.proxy                = acc->proxy;

    const std::string aid = id;
    SAM_LOG_INFO("tradelink: fetch requested for '{}'", login_name);

    job_pump::submit([this, aid, login_name, creds, cached_url, now, toast]() mutable {
        http::ScopedProxy proxy_guard(std::string(creds.proxy.data(), creds.proxy.size()));

        // The privacy page only needs a normal web session (no step-up auth), so a
        // refreshed steamLoginSecure cookie is enough.
        std::string err;
        std::string url;
        if (!steam_login::ensure_web_session(creds)) {
            SAM_LOG_WARN("tradelink: no web session for '{}'", login_name);
            err = "sign-in failed";
        } else {
            url = core::trade::fetch_trade_link(creds, &err);
        }

        // Carry the (possibly refreshed) session back to persist it.
        crypto::SecureString rt = creds.refresh_token;
        crypto::SecureString at = creds.access_token;
        crypto::SecureString ls = creds.steam_login_secure;
        std::string sid = creds.session_id;
        const std::int64_t exp = creds.access_token_expires;

        post_ui_callback([this, aid, login_name, url, err, cached_url, now, toast,
                          rt = std::move(rt), at = std::move(at), ls = std::move(ls),
                          sid = std::move(sid), exp]() mutable {
            trade_link_fetching_ids.erase(aid);
            auto* a = find_account(aid);
            if (!a) return;
            if (!rt.empty()) a->refresh_token = std::move(rt);
            if (!at.empty()) { a->access_token = std::move(at); a->access_token_expires = exp; }
            if (!ls.empty()) a->steam_login_secure = std::move(ls);
            if (!sid.empty()) a->session_id = std::move(sid);

            if (!url.empty()) {
                a->trade_url = url;
                a->trade_url_fetched_unix = now;
                vault_dirty = true;
                save_vault_if_dirty();
                platform::clipboard::set_text(url);
                SAM_LOG_INFO("tradelink: '{}' fetched and copied", login_name);
                toast(aid, login_name + ": trade link copied", false);
            } else if (!cached_url.empty()) {
                // Re-fetch failed but we have an older link; better than nothing.
                platform::clipboard::set_text(cached_url);
                toast(aid, login_name + ": using cached trade link", true);
            } else {
                toast(aid, "Trade link: fetch failed (" +
                          (err.empty() ? std::string("unknown") : err) + ")", true);
            }
        });
    });
}

void AppState::tally_spend_progress() {
    if (spend_total.load(std::memory_order_relaxed) <= 0) return;  // no bulk armed
    const int done = spend_done.fetch_add(1, std::memory_order_relaxed) + 1;
    if (done >= spend_total.load(std::memory_order_relaxed)) {
        spend_total.store(0, std::memory_order_relaxed);
        spend_done.store(0, std::memory_order_relaxed);
    }
}

void AppState::refresh_all_spend(bool only_missing) {
    if (spend_bulk_running.load(std::memory_order_acquire)) {
        SAM_LOG_INFO("spend: bulk refresh already running, ignoring");
        return;
    }

    // Eligible = full-access account with stored credentials and a SteamID.
    // only_missing skips accounts that already have a figure (the launch fill); otherwise the
    // toolbar button is cache-gated -- skip accounts whose funds were fetched within the Steam
    // cache window so a click only signs in to the ones that are actually stale.
    const auto spend_now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::int64_t spend_ttl = static_cast<std::int64_t>(settings.steam_cache_hours) * 3600;
    std::vector<std::string> ids;
    for (const auto& a : vault.accounts) {
        if (a.is_nfa || a.password.empty() || a.steam_id_64 == 0) continue;
        if (only_missing && a.funds.total_spend_usd_cents >= 0) continue;
        if (!only_missing && a.funds.last_refreshed_unix != 0 &&
            spend_now - a.funds.last_refreshed_unix < spend_ttl)
            continue;  // funds still fresh
        ids.push_back(a.id);
    }
    if (ids.empty()) {
        if (!only_missing) {
            ui::widgets::ToastItem t;
            t.id = "spend-bulk";
            t.message = "Spend is up to date for all accounts";
            t.is_warning = true;
            t.expires_at_unix = spend_now + settings.notifications.toast_duration_seconds;
            toasts.push(std::move(t));
        }
        return;
    }

    if (!only_missing) {
        ui::widgets::ToastItem t;
        t.id = "spend-bulk";
        t.message = "Fetching external funds for " + std::to_string(ids.size()) +
                    " accounts (signs in to each)...";
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        t.expires_at_unix = now + settings.notifications.toast_duration_seconds;
        toasts.push(std::move(t));
    }

    SAM_LOG_INFO("spend: bulk refresh queuing {} accounts (only_missing={})",
                 ids.size(), only_missing);
    spend_bulk_running.store(true, std::memory_order_release);
    // Arm the per-account progress counter (Fetching funds N/Total). refresh_spend bumps
    // spend_done as each account finishes and clears the pair on the last one.
    spend_total.store(static_cast<int>(ids.size()), std::memory_order_relaxed);
    spend_done.store(0, std::memory_order_relaxed);

    // One outer worker paces per-account fetches seconds apart so we never fire
    // many sign-ins at once. Each refresh_spend runs quiet (no per-account toasts).
    job_pump::submit([this, ids = std::move(ids)] {
        for (const auto& aid : ids) {
            {
                std::lock_guard lk(job_mutex);
                completed_jobs.push_back({"", [this, aid] {
                    refresh_spend(aid, /*quiet=*/true);
                }});
            }
            if (!job_pump::interruptible_sleep(std::chrono::seconds(4))) break;
        }
        spend_bulk_running.store(false, std::memory_order_release);
    });
}

namespace {
// If Steam's ConnectCache holds a valid, unexpired, client-scoped token for this account
// that differs from the stored one, copies it into `dest`/`dest_expires` and returns true.
// `dest` is the account's refresh_token (NFA) or cm_refresh_token (full-access token
// injection) -- the validity rules are identical, only the destination differs.
// Writes to the account, so call it on the UI thread only.
bool adopt_connect_cache_token_into(core::Account& a, const std::string& login_lower,
                                    crypto::SecureString& dest,
                                    std::int64_t& dest_expires) {
    auto fresh = steam_local::read_connect_cache_token(login_lower);
    if (!fresh || fresh->empty()) return false;
    const std::int64_t exp = steam_login::jwt_expiry(*fresh);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (exp != 0 && exp <= now) return false;
    if (steam_login::jwt_steam_id(*fresh) != a.steam_id_64) return false;
    if (steam_login::jwt_audience(*fresh).find("client") == std::string::npos) return false;
    if (dest == *fresh) return false;  // unchanged; Steam reused the token
    dest = std::move(*fresh);
    dest_expires = exp;
    return true;
}

bool adopt_connect_cache_token(core::Account& a, const std::string& login_lower) {
    return adopt_connect_cache_token_into(a, login_lower, a.refresh_token,
                                          a.refresh_token_expires);
}

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Waits for Steam to report this account as signed in. Job-pump thread only.
enum class SignInPoll { SignedIn, NeverSignedIn, Aborted };
SignInPoll poll_for_signin(std::uint32_t target) {
    using namespace std::chrono;
    constexpr int kMaxPolls = 90;  // up to ~90s for Steam to complete the token sign-in
    for (int i = 0; i < kMaxPolls; ++i) {
        if (!job_pump::interruptible_sleep(seconds(1))) return SignInPoll::Aborted;
        const auto au = platform::registry::read_active_user();
        if (au && *au == target) return SignInPoll::SignedIn;
    }
    return SignInPoll::NeverSignedIn;
}

// A first login defers some settings to a reapply that restarts Steam, and Steam rotates the
// refresh token on every sign-in - so reading the token back before that restart's sign-in
// stores a value the relaunch immediately supersedes, and the next launch drops to the login
// form. Wait the restart out first. False if the app is closing. Job-pump thread only.
bool wait_out_first_login_reapply() {
    using namespace std::chrono;
    constexpr int kMaxPolls = 300;  // the reapply is bounded well below this itself
    for (int i = 0; i < kMaxPolls; ++i) {
        if (!launch::first_login_reapply::in_flight()) return true;
        if (!job_pump::interruptible_sleep(seconds(1))) return false;
    }
    SAM_LOG_WARN("token read-back: first-login reapply still in flight; reading anyway");
    return true;
}
}  // namespace

void AppState::capture_rotated_token_now(const std::string& account_id,
                                         const std::string& login_lower) {
    auto* a = find_account(account_id);
    if (!a || a->steam_id_64 == 0 || login_lower.empty()) return;
    if (adopt_connect_cache_token(*a, login_lower)) {
        vault_dirty = true;
        save_vault_if_dirty();
    }
}

void AppState::capture_rotated_token_async(std::string account_id,
                                           std::uint64_t steam_id_64,
                                           std::string login_lower) {
    if (account_id.empty() || steam_id_64 == 0 || login_lower.empty()) return;
    // Steam sets ActiveUser to the account id (low 32 bits of the SteamID) once signed in.
    const auto target = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);

    job_pump::submit([this, account_id = std::move(account_id), target,
                      login_lower = std::move(login_lower)]() mutable {
        using namespace std::chrono;
        const auto poll = poll_for_signin(target);
        if (poll == SignInPoll::Aborted) return;  // app closing

        if (poll == SignInPoll::NeverSignedIn) {
            // Steam never signed in: the token is likely stale/rotated. Tell the user.
            post_ui_callback([this, account_id] {
                if (!find_account(account_id)) return;  // account removed meanwhile
                ui::widgets::ToastItem t;
                t.id = "token-stale-" + account_id;
                t.message = "Steam didn't sign in - the login token may be stale or "
                            "rotated. Re-import a fresh token.";
                t.account_id = account_id;
                t.is_warning = true;
                const auto now = duration_cast<seconds>(
                    system_clock::now().time_since_epoch()).count();
                t.expires_at_unix = now + settings.notifications.toast_duration_seconds;
                toasts.push(std::move(t));
            });
            return;
        }

        // Signed in: let any first-login restart finish, let Steam flush the rotated token to
        // local.vdf, then adopt it.
        if (!wait_out_first_login_reapply()) return;
        if (!job_pump::interruptible_sleep(seconds(3))) return;
        post_ui_callback([this, account_id, login_lower] {
            auto* a = find_account(account_id);
            if (!a) return;
            if (adopt_connect_cache_token(*a, login_lower)) {
                vault_dirty = true;
                save_vault_if_dirty();
                SAM_LOG_INFO("token self-heal: updated refresh_token for '{}'", a->login);
            }
        });
    });
}

void AppState::verify_cm_token_signin_async(std::string account_id,
                                            std::uint64_t steam_id_64,
                                            std::string login_lower) {
    if (account_id.empty() || steam_id_64 == 0 || login_lower.empty()) return;
    const auto target = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);

    job_pump::submit([this, account_id = std::move(account_id), target,
                      login_lower = std::move(login_lower)]() mutable {
        using namespace std::chrono;
        const auto poll = poll_for_signin(target);
        if (poll == SignInPoll::Aborted) return;  // app closing

        if (poll == SignInPoll::NeverSignedIn) {
            // A revoked token is accepted by every JWT check and then silently rejected by
            // Steam, so "never signed in" is the only signal we get. Mark it and re-mint.
            post_ui_callback([this, account_id] {
                auto* a = find_account(account_id);
                if (a == nullptr) return;  // account removed meanwhile
                a->cm_status = core::NfaTokenStatus::Revoked;
                a->cm_last_validated_unix = now_unix();
                vault_dirty = true;
                save_vault_if_dirty();
                SAM_LOG_WARN("cm-token: Steam never signed in for '{}' - marking revoked",
                             a->login);
                remint_cm_token_after_revoke(account_id);
            });
            return;
        }

        // Signed in: let any first-login restart finish, let Steam flush the rotated token to
        // local.vdf, then adopt it. Steam rotates the token it was handed, so without this the
        // stored cm_refresh_token drifts from what Steam holds and eventually stops working.
        if (!wait_out_first_login_reapply()) return;
        if (!job_pump::interruptible_sleep(seconds(3))) return;
        post_ui_callback([this, account_id, login_lower] {
            auto* a = find_account(account_id);
            if (a == nullptr) return;
            a->cm_status = core::NfaTokenStatus::Valid;
            a->cm_last_validated_unix = now_unix();
            cm_relaunch_attempted.erase(account_id);
            const bool rotated = adopt_connect_cache_token_into(
                *a, login_lower, a->cm_refresh_token, a->cm_refresh_token_expires);
            if (rotated) {
                SAM_LOG_INFO("cm-token self-heal: updated cm_refresh_token for '{}'", a->login);
            }
            vault_dirty = true;
            save_vault_if_dirty();
        });
    });
}

void AppState::remint_cm_token_after_revoke(const std::string& account_id) {
    auto* a = find_account(account_id);
    if (a == nullptr) return;

    const auto warn = [&](std::string msg) {
        ui::widgets::ToastItem t;
        t.id = "cm-token-dead-" + account_id;
        t.message = std::move(msg);
        t.account_id = account_id;
        t.is_warning = true;
        t.expires_at_unix = now_unix() + settings.notifications.toast_duration_seconds;
        toasts.push(std::move(t));
    };
    const std::string who = a->web.persona_name.empty() ? a->login : a->web.persona_name;

    if (pending_token_launch.minting) return;  // a mint is already in flight; don't burn the try

    // Once per user-initiated launch: a re-mint that still can't sign in means something we
    // can't fix from here (password changed, Guard removed), so stop rather than loop.
    if (!cm_relaunch_attempted.insert(account_id).second) {
        warn("Steam rejected the login token for " + who +
             " again - check the account's password and Steam Guard.");
        return;
    }
    if (a->password.empty()) {
        warn("Login token for " + who +
             " was rejected and there's no stored password to mint a new one.");
        return;
    }

    warn("Login token for " + who + " was rejected - minting a new one and retrying...");
    pending_token_launch = {account_id, true, false, false, {}};
    acquire_cm_token(account_id, [this](bool ok, std::string err) {
        pending_token_launch.minting = false;
        pending_token_launch.mint_done = true;
        pending_token_launch.mint_ok = ok;
        pending_token_launch.mint_error = std::move(err);
    });
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

}  // namespace sam::app
