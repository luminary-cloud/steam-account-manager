#include "core/launch/token_launcher.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "core/launch/steam_launcher.hpp"
#include "core/log.hpp"
#include "core/steam_local/connect_cache.hpp"
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

std::string to_lower_ascii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
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

LaunchResult launch_account_with_token(const core::Account& a) {
    if (a.refresh_token.empty() || a.steam_id_64 == 0)
        return fail("NFA login needs a refresh token and a resolved Steam ID.");
    if (a.login.empty())
        return fail("NFA login needs the account name; edit the account and set it.");

    const std::int64_t exp = steam_login::jwt_expiry(a.refresh_token);
    if (exp != 0 && exp <= now_seconds())
        return fail("The NFA token is expired; import a fresh one.");
    if (steam_login::jwt_audience(a.refresh_token).find("client") == std::string::npos)
        return fail("This token can't sign into the Steam client (no \"client\" audience).");

    LaunchResult out;
    auto steam_exe = resolve_steam_exe(out);
    if (!steam_exe) return out;                 // out carries the SteamNotInstalled reason
    if (!shutdown_running_steam(*steam_exe, out)) return out;

    const std::string login = to_lower_ascii(a.login);

    platform::registry::set_auto_login_user(widen_ascii(login));
    steam_local::ensure_loginusers_entry(a.steam_id_64, login, a.web.persona_name);
    if (!steam_local::write_connect_cache_token(login, a.refresh_token))
        return fail("Could not write the login token to Steam's local.vdf.");

    auto pid = platform::process::launch(*steam_exe, L"");
    if (!pid) {
        out.status = LaunchStatus::SpawnFailed;
        out.message = "CreateProcess for steam.exe failed";
        return out;
    }

    SAM_LOG_INFO("token-launch: started steam.exe pid={} as login={}", *pid, login);
    return out;
}

}  // namespace sam::launch
