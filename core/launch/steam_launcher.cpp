#include "core/launch/steam_launcher.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/cs2_config/launch_options.hpp"
#include "core/cs2_config/workshop_block.hpp"
#include "core/launch/first_login_reapply.hpp"
#include "core/launch/hwid_inject.hpp"
#include "core/launch/login_driver.hpp"
#include "core/launch/token_launcher.hpp"
#include "core/log.hpp"
#include "core/steam_local/login_prefs.hpp"
#include "core/strings.hpp"
#include "platform/process.hpp"
#include "platform/registry.hpp"

namespace sam::launch {

namespace {
// Steam writes local.vdf/config.vdf as both steam.exe and steamwebhelper.exe exit, so a
// shutdown has to wait for both before we touch those files, or a late write from a
// lingering helper overwrites ours. steamservice.exe is a long-running background service,
// so it's left alone.
std::vector<std::uint32_t> steam_processes_running() {
    std::vector<std::uint32_t> pids;
    for (const wchar_t* name : {L"steam.exe", L"steamwebhelper.exe"}) {
        auto found = platform::process::find_by_image_name(name);
        pids.insert(pids.end(), found.begin(), found.end());
    }
    return pids;
}
}  // namespace

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
    if (steam_processes_running().empty()) return true;

    platform::process::launch(exe_path, L"-shutdown");
    // Wait for every client process to go, not just steam.exe. A helper still writing its
    // config after we return would overwrite the token we inject next.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (steam_processes_running().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    // Force-kill any stragglers (best-effort; a child may already be exiting), then verify.
    for (auto pid : steam_processes_running())
        platform::process::terminate(pid);
    if (!steam_processes_running().empty()) {
        out.status = LaunchStatus::KillFailed;
        out.message = "could not terminate running Steam processes";
        return false;
    }
    // Give the OS a moment to finish flushing Steam's config to disk before callers edit
    // local.vdf/config.vdf, so our writes land last. The first-login reapply path waits the
    // same way for the same reason.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    return true;
}

LaunchResult launch_account(const core::Account& a, std::string_view cs2_launch_options,
                            bool disable_cloud_on_login, bool disable_news_on_login,
                            bool disable_workshop_on_login, bool remember_password,
                            std::filesystem::path gamesense_loader,
                            std::filesystem::path luminary_loader,
                            std::uint32_t hwid_component_mask, bool use_token) {
    if (a.is_nfa) {
        return launch_account_with_token(a, cs2_launch_options, disable_cloud_on_login,
                                         disable_news_on_login, disable_workshop_on_login,
                                         hwid_component_mask, gamesense_loader, luminary_loader);
    }
    if (use_token) {
        core::Account token_view = a;
        token_view.refresh_token = a.cm_refresh_token;
        return launch_account_with_token(token_view, cs2_launch_options, disable_cloud_on_login,
                                         disable_news_on_login, disable_workshop_on_login,
                                         hwid_component_mask, gamesense_loader, luminary_loader);
    }

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
    // A first login (no per-user config yet) is initialized from scratch by Steam, which
    // overwrites anything we pre-write while it was down. Detect that per setting and defer
    // it: re-apply after the sign-in via on_login_confirmed below. When the file already
    // exists the in-place edit survives, so keep the pre-write + single launch (no restart).
    //
    // The deferred path restarts Steam after sign-in. For CS2-autostart methods the reapply
    // callback resumes the CS2 launch itself once Steam is back up (see below), so the
    // restart no longer races the auto-launch and every login method can defer.
    const auto presence = steam_local::login_config_presence(a.steam_id_64);
    const bool defer_news = disable_news_on_login && !presence.localconfig_present;
    const bool defer_cloud = disable_cloud_on_login && !presence.sharedconfig_present;
    // LaunchOptions live in localconfig.vdf, so the first-login wipe hits them too.
    const bool defer_launch_options =
        !cs2_launch_options.empty() && !presence.localconfig_present;

    if (disable_news_on_login && !defer_news) {
        const auto r = steam_local::set_news_notify_off(a.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("launch: disable news: {}", r.message);
    }
    if (disable_cloud_on_login && !defer_cloud) {
        const auto r = steam_local::set_cloud_enabled_off(a.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("launch: disable cloud: {}", r.message);
    }
    // Block/unblock CS2 workshop-map downloads per the global toggle. appworkshop_730.acf
    // lives under steamapps/ (not per-user), so it isn't clobbered by a first login and
    // needs no defer.
    {
        const auto r = disable_workshop_on_login
                           ? cs2_config::apply_workshop_block(a.steam_id_64)
                           : cs2_config::unlock_workshop_block();
        if (!r.ok) SAM_LOG_WARN("launch: workshop block: {}", r.message);
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

    auto hwid_res = maybe_inject_hwid(a, *pid, hwid_component_mask);
    out.hwid_outcome = hwid_res.outcome;
    out.hwid_error = std::move(hwid_res.error);

    login_driver::Credentials creds;
    creds.login = sam::crypto::make_secure(a.login);
    creds.password = a.password;
    if (a.sda) creds.shared_secret = a.sda->shared_secret;
    creds.remember_password = remember_password;
    creds.expected_account_id = static_cast<std::uint32_t>(a.steam_id_64 & 0xFFFFFFFFull);

    if (defer_news || defer_cloud || defer_launch_options) {
        out.first_login_deferred = true;
        first_login_reapply::Params p;
        p.steam_exe = *exe_path;
        p.steam_id_64 = a.steam_id_64;
        p.login_lower = core::to_lower(a.login);
        // string_view-backed; copy so it outlives this call into the async callback.
        p.cs2_launch_options = std::string(cs2_launch_options);
        p.apply_launch_options = defer_launch_options;
        p.apply_news = defer_news;
        p.apply_cloud = defer_cloud;
        // The driver already confirms the sign-in before firing the callback.
        p.sign_in_timeout_seconds = 0;
        // CS2-autostart methods resume their launch from inside the reapply, after the
        // relaunch, so it doesn't race the restart.
        p.method = a.login_method;
        p.gamesense_loader = std::move(gamesense_loader);
        p.luminary_loader = std::move(luminary_loader);
        p.hwid = a.hwid;
        p.hwid_mask = hwid_component_mask;
        creds.on_login_confirmed = [p](const std::function<bool()>& still_current) {
            first_login_reapply::run(p, still_current);
        };
    }

    out.guard_code_was_typed = login_driver::run_async(*pid, std::move(creds));

    SAM_LOG_INFO("launch: started steam.exe pid={} as login={}", *pid, a.login);
    return out;
}

}  // namespace sam::launch
