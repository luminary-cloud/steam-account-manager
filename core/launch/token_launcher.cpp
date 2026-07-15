#include "core/launch/token_launcher.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "core/cs2_config/launch_options.hpp"
#include "core/cs2_config/workshop_block.hpp"
#include "core/launch/hwid_inject.hpp"
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

// Steam account names are ASCII, so a byte-wise widen is sufficient.
std::wstring widen_ascii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

LaunchResult fail(const std::string& message) {
    LaunchResult out;
    out.status = LaunchStatus::SpawnFailed;
    out.message = message;
    return out;
}

}  // namespace

LaunchResult launch_account_with_token(const core::Account& a,
                                       std::string_view cs2_launch_options,
                                       bool disable_cloud_on_login,
                                       bool disable_news_on_login,
                                       bool disable_workshop_on_login,
                                       std::uint32_t hwid_component_mask) {
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
    if (!steam_exe) return out;                 // out carries the SteamNotInstalled reason
    if (!shutdown_running_steam(*steam_exe, out)) return out;

    // Steam is down now: safe to set launch options without Steam clobbering them.
    if (!cs2_launch_options.empty()) {
        const auto r =
            cs2_config::apply_launch_options(a.steam_id_64, std::string(cs2_launch_options));
        if (!r.ok) SAM_LOG_WARN("token-launch: cs2 launch options: {}", r.message);
    }
    if (disable_news_on_login) {
        const auto r = steam_local::set_news_notify_off(a.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("token-launch: disable news: {}", r.message);
    }
    if (disable_cloud_on_login) {
        const auto r = steam_local::set_cloud_enabled_off(a.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("token-launch: disable cloud: {}", r.message);
    }
    // Block/unlock CS2 workshop-map downloads for this account per the global toggle.
    {
        const auto r = disable_workshop_on_login
                           ? cs2_config::apply_workshop_block(a.steam_id_64)
                           : cs2_config::unlock_workshop_block();
        if (!r.ok) SAM_LOG_WARN("token-launch: workshop block: {}", r.message);
    }

    const std::string login = core::to_lower(a.login);

    platform::registry::set_auto_login_user(widen_ascii(login));
    steam_local::ensure_loginusers_entry(a.steam_id_64, login, a.web.persona_name);
    if (!steam_local::write_connect_cache_token(login, a.refresh_token))
        return fail("Could not write the login token to Steam's local.vdf.");
    // Steam maps the AutoLoginUser name to its Steam ID via config.vdf; without
    // this entry it ignores the injected token and shows the login window.
    if (!steam_local::ensure_config_vdf_account(a.steam_id_64, login))
        SAM_LOG_WARN("token-launch: config.vdf account entry not written");
    // "Ask which account to use each time Steam starts" (AlwaysShowUserChooser) makes
    // Steam show the picker and ignore AutoLoginUser, stalling the token sign-in. Clear
    // it now while Steam is shut down so the launch below signs in silently.
    if (!steam_local::disable_account_chooser())
        SAM_LOG_WARN("token-launch: could not clear AlwaysShowUserChooser in config.vdf");

    auto pid = platform::process::launch(*steam_exe, L"");
    if (!pid) {
        out.status = LaunchStatus::SpawnFailed;
        out.message = "CreateProcess for steam.exe failed";
        return out;
    }

    auto hwid_res = maybe_inject_hwid(a, *pid, hwid_component_mask);
    out.hwid_outcome = hwid_res.outcome;
    out.hwid_error = std::move(hwid_res.error);

    SAM_LOG_INFO("token-launch: started steam.exe pid={} as login={}", *pid, login);
    return out;
}

}  // namespace sam::launch
