#include "app/app_state.hpp"
#include "app/app_state_internal.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "app/app_paths.hpp"
#include "app/job_pump.hpp"
#include "core/cs2_config/video_config.hpp"
#include "core/crypto/rng.hpp"
#include "core/http/client.hpp"
#include "core/http/url.hpp"
#include "core/log.hpp"
#include "core/steam_login/browser_login.hpp"
#include "core/steam_login/mobile_auth.hpp"
#include "core/steam_login/session.hpp"
#include "platform/browser.hpp"

namespace sam::app {

bool AppState::auto_relogin(const std::string& account_id,
                            core::Account& creds,
                            std::string* err) {
    auto set_err = [&](std::string msg) { if (err) *err = std::move(msg); };
    http::ScopedProxy proxy_guard(std::string(creds.proxy.data(), creds.proxy.size()));

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
    // run_full_login ships back steam_login_secure + session_id bound to the
    // registered community session. If settoken failed the cookie is the manual
    // fallback, and mobileconf writes trip success=false until the next relogin.
    creds.steam_login_secure   = std::move(result.account.steam_login_secure);
    if (!result.account.session_id.empty()) {
        creds.session_id = std::move(result.account.session_id);
    } else if (creds.session_id.empty()) {
        creds.session_id = crypto::random_session_id();
    }
    return true;
}

void AppState::acquire_cm_token(const std::string& account_id,
                                std::function<void(bool, std::string)> on_done) {
    core::Account* a = find_account(account_id);
    if (a == nullptr) {
        if (on_done) on_done(false, "account not found");
        return;
    }
    if (a->login.empty() || a->password.empty()) {
        if (on_done) on_done(false, "no stored password for this account");
        return;
    }

    const std::string aid = account_id;
    core::Account creds;
    creds.login = a->login;
    creds.password = a->password;
    creds.sda = a->sda;
    creds.proxy = a->proxy;

    job_pump::submit([this, aid, creds, on_done = std::move(on_done)]() mutable {
        http::ScopedProxy proxy_guard(std::string(creds.proxy.data(), creds.proxy.size()));
        steam_login::MobileLogin login;
        login.username = creds.login;
        login.password = creds.password;
        auto sda = creds.sda;
        auto result = steam_login::acquire_client_token(
            login,
            [&](const std::vector<steam_login::GuardKind>& allowed) {
                return steam_login::default_guard_provider(sda, allowed, /*on_prompt=*/{});
            },
            [&](const std::string& s) {
                SAM_LOG_INFO("cs2 client-login: {} ({})", s, creds.login);
            });

        if (!result.ok) {
            SAM_LOG_WARN("cs2 client-login: failed for '{}': {}", creds.login, result.error);
            post_ui_callback([on_done = std::move(on_done), err = result.error]() mutable {
                if (on_done) on_done(false, err.empty() ? std::string{"Steam declined the sign-in"} : err);
            });
            return;
        }

        crypto::SecureString rt = std::move(result.refresh_token);
        const std::int64_t exp = result.expires;
        post_ui_callback([this, aid, rt = std::move(rt), exp, on_done = std::move(on_done)]() mutable {
            if (auto* acc = find_account(aid)) {
                acc->cm_refresh_token = std::move(rt);
                acc->cm_refresh_token_expires = exp;
                // Freshly minted: any Revoked verdict belonged to the token we just
                // replaced, and this one is unproven until a sign-in says otherwise.
                acc->cm_status = core::NfaTokenStatus::Unknown;
                acc->cm_last_validated_unix = 0;
                vault_dirty = true;
                save_vault_if_dirty();
            }
            if (on_done) on_done(true, std::string{});
        });
    });
}

namespace {
// UI thread only (touches `toasts`).
void push_cs2_toast(AppState& state, const std::string& aid,
                    const std::string& login,
                    const cs2_config::DeployResult& result, const char* prefix) {
    const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ui::widgets::ToastItem t;
    t.id = "cs2cfg-" + aid + "-" + std::to_string(now_unix);
    t.message = result.ok ? (std::string(prefix) + " applied")
                          : (std::string(prefix) + ": " + result.message);
    t.account_id = aid;
    t.is_warning = !result.ok;
    t.expires_at_unix = now_unix + state.settings.notifications.toast_duration_seconds;
    state.toasts.push(std::move(t));

    if (!result.ok) {
        SAM_LOG_WARN("cs2 config: {} (login={})", result.message, login);
    }
}
}  // namespace

void AppState::apply_cs2_video_config(const core::Account& a) {
    const auto mode = settings.cs2_video.mode;
    if (mode == CS2ConfigMode::None) return;

    // By value: the Account& may be invalidated by a vault mutation before a job runs.
    const std::string aid = a.id;
    const std::string login = a.login;
    const std::uint64_t sid = a.steam_id_64;

    if (mode == CS2ConfigMode::VideoTxt) {
        const auto result =
            cs2_config::deploy_video_config(sid, cs2_video_template_path());
        push_cs2_toast(*this, aid, login, result, "CS2 video config");
        return;
    }

    // Folder730 recursive copy can be large; run off the UI thread, marshal the
    // toast back.
    job_pump::submit([this, aid, login, sid, tdir = cs2_730_template_dir()]() {
        const auto result = cs2_config::deploy_730_folder(sid, tdir);
        post_ui_callback([this, aid, login, result]() {
            push_cs2_toast(*this, aid, login, result, "CS2 730 folder");
        });
    });
}

void AppState::open_account_in_browser(const core::Account& a) {
    // NFA can't mint a web session; no SteamID = no profile. Context menu disables
    // both, but guard again here.
    if (a.steam_id_64 == 0 || a.is_nfa) return;

    const std::string aid = a.id;
    const std::string login_name = a.login;
    const std::uint64_t sid64 = a.steam_id_64;

    // Thin copy for the worker; the Account& may be invalidated before the job runs.
    core::Account creds;
    creds.steam_id_64 = a.steam_id_64;
    creds.login = a.login;
    creds.session_id = a.session_id;
    creds.steam_login_secure = a.steam_login_secure;
    creds.refresh_token = a.refresh_token;
    creds.access_token = a.access_token;
    creds.access_token_expires = a.access_token_expires;
    creds.password = a.password;
    creds.sda = a.sda;
    creds.proxy = a.proxy;

    SAM_LOG_INFO("browser-login: requested for '{}' (steam_id={})", login_name, sid64);

    job_pump::submit([this, aid, login_name, sid64, creds]() mutable {
        http::ScopedProxy proxy_guard(std::string(creds.proxy.data(), creds.proxy.size()));

        if (creds.session_id.empty()) creds.session_id = crypto::random_session_id();

        // finalizelogin consumes the refresh_token. Refresh the cheap access_token
        // first; if the refresh_token is missing or rejected, full re-login.
        if (creds.refresh_token.empty()) {
            std::string err;
            auto_relogin(aid, creds, &err);
        } else if (steam_login::needs_refresh(creds, 300)) {
            if (!steam_login::refresh_access_token(creds)) {
                std::string err;
                auto_relogin(aid, creds, &err);
            }
        }

        const std::string profile_url =
            "https://steamcommunity.com/profiles/" + std::to_string(sid64);
        const std::string redir = "https://steamcommunity.com/login/home/?goto=" +
            http::url_encode("profiles/" + std::to_string(sid64));

        auto build_targets = [&](std::vector<steam_login::TransferTarget>& out) {
            return steam_login::finalize_login_targets(
                creds.steam_id_64, creds.refresh_token, creds.session_id, redir, out);
        };

        std::vector<steam_login::TransferTarget> targets;
        if (!build_targets(targets)) {
            // refresh_token may be expired; one re-login + retry.
            std::string err;
            if (auto_relogin(aid, creds, &err)) build_targets(targets);
        }

        // Replicate Steam's per-domain transfer: each target's cookies are set in
        // hidden frames, then the page lands on the profile.
        bool ready = false;
        if (!targets.empty()) {
            const std::string html =
                steam_login::build_login_html(targets, sid64, profile_url);
            if (!html.empty()) {
                std::error_code mkec;
                std::filesystem::create_directories(
                    browser_login_html_path().parent_path(), mkec);
                std::ofstream f(browser_login_html_path(), std::ios::binary | std::ios::trunc);
                if (f) {
                    f.write(html.data(), static_cast<std::streamsize>(html.size()));
                    ready = static_cast<bool>(f);
                }
            }
        }
        if (!ready) {
            SAM_LOG_WARN("browser-login: could not mint a web session for '{}'", login_name);
        }

        // Carry rotated tokens back to persist them.
        crypto::SecureString rt = creds.refresh_token;
        crypto::SecureString at = creds.access_token;
        crypto::SecureString ls = creds.steam_login_secure;
        std::string ses = creds.session_id;
        const std::int64_t exp = creds.access_token_expires;

        post_ui_callback([this, aid, login_name, ready,
                          rt = std::move(rt), at = std::move(at), ls = std::move(ls),
                          ses = std::move(ses), exp]() mutable {
            if (auto* acc = find_account(aid)) {
                if (!rt.empty()) acc->refresh_token = std::move(rt);
                if (!at.empty()) { acc->access_token = std::move(at); acc->access_token_expires = exp; }
                if (!ls.empty()) acc->steam_login_secure = std::move(ls);
                if (!ses.empty()) acc->session_id = std::move(ses);
                vault_dirty = true;
                save_vault_if_dirty();
            }

            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            ui::widgets::ToastItem t;
            t.id = "browser-login-" + aid;
            t.account_id = aid;
            t.expires_at_unix = now + settings.notifications.toast_duration_seconds;

            if (!ready) {
                t.message = "Browser sign-in failed for " + login_name +
                            " (couldn't mint a web session)";
                t.is_warning = true;
                toasts.push(std::move(t));
                return;
            }

            std::error_code ec;
            std::filesystem::create_directories(browser_profile_dir(), ec);
            const std::wstring url = L"file:///" + browser_login_html_path().generic_wstring();
            const bool isolated =
                platform::open_isolated_window(url, browser_profile_dir().wstring());
            t.message = isolated
                ? "Opened " + login_name + " in an isolated browser window"
                : "Opened " + login_name + " (couldn't isolate - used default browser)";
            t.is_warning = !isolated;
            toasts.push(std::move(t));
        });
    });
}

}  // namespace sam::app
