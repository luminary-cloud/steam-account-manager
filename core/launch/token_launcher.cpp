#include "core/launch/token_launcher.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "core/cs2_config/workshop_block.hpp"
#include "core/launch/first_login_reapply.hpp"
#include "core/launch/hwid_inject.hpp"
#include "core/launch/launch_gen.hpp"
#include "core/launch/steam_launcher.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/steam_local/connect_cache.hpp"
#include "core/steam_local/login_prefs.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/steam_login/session.hpp"
#include "platform/process.hpp"
#include "platform/registry.hpp"

namespace sam::launch {

namespace {

std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::wstring widen_ascii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

LaunchResult fail(const std::string& message) {
    LaunchResult out;
    out.status = LaunchStatus::SpawnFailed;
    out.message = message;
    return out;
}

constexpr int kTokenSignInTimeoutSeconds = 180;

}  // namespace

LaunchResult launch_account_with_token(const core::Account& a, const LaunchOptions& opts) {
    if (a.refresh_token.empty() || a.steam_id_64 == 0)
        return fail("Token login needs a refresh token and a resolved Steam ID.");
    if (a.login.empty())
        return fail("Token login needs the account name; edit the account and set it.");

    const std::int64_t exp = steam_login::jwt_expiry(a.refresh_token);
    if (exp != 0 && exp <= now_seconds())
        return fail("The login token is expired; re-mint or import a fresh one.");
    if (steam_login::jwt_audience(a.refresh_token).find("client") == std::string::npos)
        return fail("This token can't sign into the Steam client (no \"client\" audience).");

    LaunchResult out;
    auto steam_exe = resolve_steam_exe(out);
    if (!steam_exe) return out;

    const std::uint64_t gen = launch_gen::begin();
    if (!shutdown_running_steam(*steam_exe, out)) return out;

    if (opts.after_steam_down) opts.after_steam_down();

    const auto presence = steam_local::login_config_presence(a.steam_id_64);
    const auto deferred =
        first_login_reapply::apply_login_prefs(a.steam_id_64, opts, presence, "token-launch");

    {
        const auto r = opts.disable_workshop_on_login
                           ? cs2_config::apply_workshop_block(a.steam_id_64)
                           : cs2_config::clear_workshop_block(a.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("token-launch: workshop block: {}", r.message);
    }

    const std::string login = core::to_lower(a.login);

    platform::registry::set_auto_login_user(widen_ascii(login));
    steam_local::ensure_loginusers_entry(a.steam_id_64, login, a.web.persona_name);
    if (!steam_local::write_connect_cache_token(login, a.refresh_token))
        return fail("Could not write the login token to Steam's local.vdf.");

    const auto injected_cc = steam_local::read_connect_cache_value(login);

    if (!steam_local::ensure_config_vdf_account(a.steam_id_64, login))
        SAM_LOG_WARN("token-launch: config.vdf account entry not written");

    if (!steam_local::disable_account_chooser())
        SAM_LOG_WARN("token-launch: could not clear AlwaysShowUserChooser in config.vdf");

    auto pid = platform::process::launch(*steam_exe, L"");
    if (!pid) {
        out.status = LaunchStatus::SpawnFailed;
        out.message = "CreateProcess for steam.exe failed";
        return out;
    }

    if (!opts.safe_mode) {
        auto hwid_res = maybe_inject_hwid(a, *pid, opts.hwid_component_mask);
        out.hwid_outcome = hwid_res.outcome;
        out.hwid_error = std::move(hwid_res.error);
    }

    SAM_LOG_INFO("token-launch: started steam.exe pid={} as login={}", *pid, login);

    if (deferred.any()) {
        out.first_login_deferred = true;
        first_login_reapply::Params p;
        p.steam_exe = *steam_exe;
        p.steam_id_64 = a.steam_id_64;
        p.login_lower = login;
        p.cs2_launch_options = opts.cs2_launch_options;
        p.deferred = deferred;

        p.sign_in_timeout_seconds = kTokenSignInTimeoutSeconds;
        p.clear_account_chooser = true;
        p.injected_connect_cache = injected_cc;

        p.method = core::effective_login_method(a.login_method, opts.safe_mode);
        p.gamesense_loader = opts.gamesense_loader;
        p.luminary_loader = opts.luminary_loader;
        p.hwid = opts.safe_mode ? std::nullopt : a.hwid;
        p.hwid_mask = opts.hwid_component_mask;

        first_login_reapply::mark_scheduled();
        std::thread([p = std::move(p), gen] {
            try {
                first_login_reapply::run(p, [gen] { return launch_gen::is_current(gen); });
            } catch (...) {
                SAM_LOG_ERROR("token-launch: first-login reapply worker threw");
            }
            first_login_reapply::clear_scheduled();
        }).detach();
    }
    return out;
}

}  // namespace sam::launch
