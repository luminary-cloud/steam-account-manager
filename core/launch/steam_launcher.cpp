#include "core/launch/steam_launcher.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (steam_processes_running().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    for (auto pid : steam_processes_running())
        platform::process::terminate(pid);
    if (!steam_processes_running().empty()) {
        out.status = LaunchStatus::KillFailed;
        out.message = "could not terminate running Steam processes";
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    return true;
}

LaunchResult launch_account(const core::Account& a, const LaunchOptions& opts) {
    if (a.is_nfa) {
        return launch_account_with_token(a, opts);
    }
    if (opts.use_token) {
        core::Account token_view = a;
        token_view.refresh_token = a.cm_refresh_token;
        return launch_account_with_token(token_view, opts);
    }

    LaunchResult out;

    auto exe_path = resolve_steam_exe(out);
    if (!exe_path) return out;
    if (!shutdown_running_steam(*exe_path, out)) return out;

    if (opts.after_steam_down) opts.after_steam_down();

    const auto presence = steam_local::login_config_presence(a.steam_id_64);
    const auto deferred =
        first_login_reapply::apply_login_prefs(a.steam_id_64, opts, presence, "launch");

    {
        const auto r = opts.disable_workshop_on_login
                           ? cs2_config::apply_workshop_block(a.steam_id_64)
                           : cs2_config::clear_workshop_block(a.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("launch: workshop block: {}", r.message);
    }

    platform::registry::set_auto_login_user(L"");
    platform::registry::set_remember_password(false);

    auto pid = platform::process::launch(*exe_path, L"");
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

    login_driver::Credentials creds;
    creds.login = sam::crypto::make_secure(a.login);
    creds.password = a.password;
    if (a.sda) creds.shared_secret = a.sda->shared_secret;
    creds.remember_password = opts.remember_password;
    creds.expected_account_id = static_cast<std::uint32_t>(a.steam_id_64 & 0xFFFFFFFFull);

    if (deferred.any()) {
        out.first_login_deferred = true;
        first_login_reapply::Params p;
        p.steam_exe = *exe_path;
        p.steam_id_64 = a.steam_id_64;
        p.login_lower = core::to_lower(a.login);
        p.cs2_launch_options = opts.cs2_launch_options;
        p.deferred = deferred;

        p.sign_in_timeout_seconds = 0;

        p.method = core::effective_login_method(a.login_method, opts.safe_mode);
        p.gamesense_loader = opts.gamesense_loader;
        p.luminary_loader = opts.luminary_loader;
        p.hwid = opts.safe_mode ? std::nullopt : a.hwid;
        p.hwid_mask = opts.hwid_component_mask;
        creds.on_login_confirmed = [p](const std::function<bool()>& still_current) {
            first_login_reapply::run(p, still_current);
        };
    }

    out.guard_code_was_typed = login_driver::run_async(*pid, std::move(creds));

    SAM_LOG_INFO("launch: started steam.exe pid={} as login={}", *pid, a.login);
    return out;
}

}  // namespace sam::launch
