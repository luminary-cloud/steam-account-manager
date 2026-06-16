#include "core/launch/steam_launcher.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

#include "core/cs2_config/launch_options.hpp"
#include "core/launch/login_driver.hpp"
#include "core/launch/token_launcher.hpp"
#include "core/log.hpp"
#include "core/steam_local/login_prefs.hpp"
#include "platform/process.hpp"
#include "platform/registry.hpp"

namespace sam::launch {

std::optional<std::filesystem::path> resolve_steam_exe(LaunchResult& out) {
    auto steam_exe = platform::registry::read_steam_exe_path();
    if (!steam_exe) {
        out.status = LaunchStatus::SteamNotInstalled;
        out.message = "Steam install path not found in registry";
        return std::nullopt;
    }

    std::filesystem::path exe_path = *steam_exe;
    if (std::filesystem::is_directory(exe_path)) {
        exe_path /= L"steam.exe";
    }
    if (!std::filesystem::exists(exe_path)) {
        out.status = LaunchStatus::SteamNotInstalled;
        out.message = "steam.exe not found at " + exe_path.string();
        return std::nullopt;
    }
    return exe_path;
}

bool shutdown_running_steam(const std::filesystem::path& exe_path, LaunchResult& out) {
    if (platform::process::find_by_image_name(L"steam.exe").empty()) return true;

    platform::process::launch(exe_path, L"-shutdown");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        if (platform::process::find_by_image_name(L"steam.exe").empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    for (auto pid : platform::process::find_by_image_name(L"steam.exe")) {
        if (!platform::process::terminate(pid)) {
            out.status = LaunchStatus::KillFailed;
            out.message = "could not terminate running steam.exe";
            return false;
        }
    }
    return true;
}

LaunchResult launch_account(const core::Account& a, std::string_view cs2_launch_options,
                            bool disable_cloud_on_login, bool disable_news_on_login) {
    // NFA accounts have no password to type; sign them in via token injection.
    if (a.is_nfa)
        return launch_account_with_token(a, cs2_launch_options, disable_cloud_on_login,
                                         disable_news_on_login);

    LaunchResult out;

    auto exe_path = resolve_steam_exe(out);
    if (!exe_path) return out;
    if (!shutdown_running_steam(*exe_path, out)) return out;

    // Steam is down now: safe to set launch options without Steam clobbering them.
    if (!cs2_launch_options.empty()) {
        const auto r =
            cs2_config::apply_launch_options(a.steam_id_64, std::string(cs2_launch_options));
        if (!r.ok) SAM_LOG_WARN("launch: cs2 launch options: {}", r.message);
    }
    if (disable_news_on_login) {
        const auto r = steam_local::set_news_notify_off(a.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("launch: disable news: {}", r.message);
    }
    if (disable_cloud_on_login) {
        const auto r = steam_local::set_cloud_enabled_off(a.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("launch: disable cloud: {}", r.message);
    }

    // Force the login window: without this, a stale AutoLoginUser auto-logs in as
    // the previous account before the driver even sees the login UI.
    platform::registry::set_auto_login_user(L"");
    platform::registry::set_remember_password(false);

    auto pid = platform::process::launch(*exe_path, L"");
    if (!pid) {
        out.status = LaunchStatus::SpawnFailed;
        out.message = "CreateProcess for steam.exe failed";
        return out;
    }

    login_driver::Credentials creds;
    creds.login = sam::crypto::make_secure(a.login);
    creds.password = a.password;
    if (a.sda) creds.shared_secret = a.sda->shared_secret;
    creds.remember_password = true;
    creds.expected_account_id = static_cast<std::uint32_t>(a.steam_id_64 & 0xFFFFFFFFull);
    out.guard_code_was_typed = login_driver::run_async(*pid, std::move(creds));

    SAM_LOG_INFO("launch: started steam.exe pid={} as login={}", *pid, a.login);
    return out;
}

}  // namespace sam::launch
