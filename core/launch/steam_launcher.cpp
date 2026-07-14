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
#include "core/launch/cs2_autostart.hpp"
#include "core/launch/hwid_inject.hpp"
#include "core/launch/login_driver.hpp"
#include "core/launch/token_launcher.hpp"
#include "core/log.hpp"
#include "core/steam_local/connect_cache.hpp"
#include "core/steam_local/login_prefs.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/strings.hpp"
#include "platform/process.hpp"
#include "platform/registry.hpp"

namespace sam::launch {

namespace {
constexpr auto kSetupPollInterval = std::chrono::milliseconds(250);
constexpr auto kMinSetupDwell     = std::chrono::seconds(5);
constexpr auto kSetupTimeout      = std::chrono::seconds(60);

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
                            bool remember_password, std::filesystem::path gamesense_loader,
                            std::filesystem::path luminary_loader,
                            std::uint32_t hwid_component_mask, bool use_token) {
    if (a.is_nfa) {
        return launch_account_with_token(a, cs2_launch_options, disable_cloud_on_login,
                                         disable_news_on_login, hwid_component_mask);
    }
    if (use_token) {
        core::Account token_view = a;
        token_view.refresh_token = a.cm_refresh_token;
        return launch_account_with_token(token_view, cs2_launch_options, disable_cloud_on_login,
                                         disable_news_on_login, hwid_component_mask);
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
        // Steam account names are ASCII, so a byte-wise widen of the lowercased login is fine.
        const std::string login_lower = core::to_lower(a.login);
        const std::wstring login_w(login_lower.begin(), login_lower.end());
        const std::filesystem::path exe = *exe_path;
        const std::uint64_t sid = a.steam_id_64;
        // string_view-backed; copy so it outlives this call into the async callback.
        const std::string opts(cs2_launch_options);
        // CS2-autostart methods resume their launch from inside the callback, after the
        // relaunch, so it doesn't race the restart.
        const core::LoginMethod method = a.login_method;
        const auto hwid_profile = a.hwid;
        const auto hwid_mask = hwid_component_mask;
        const std::filesystem::path loader = std::move(gamesense_loader);
        const std::filesystem::path lum_loader = std::move(luminary_loader);
        creds.on_login_confirmed =
            [exe, login_w, login_lower, sid, opts, method, hwid_profile, hwid_mask, loader,
             lum_loader, defer_news, defer_cloud, defer_launch_options](
                const std::function<bool()>& still_current) {
                // ActiveUser flips the moment Steam authenticates, long before it creates
                // the account's userdata or writes its login state (loginusers/config.vdf),
                // so stopping Steam right away kills it mid-setup and the relaunch has
                // nothing to sign in with (blank login window). Wait until Steam has set up
                // the account's userdata, grabbing the remember-me token it stored along the
                // way. First login only, so this one-time wait is acceptable.
                SAM_LOG_INFO("first-login reapply: waiting for Steam to finish first-login "
                             "setup before restart");
                std::optional<std::string> cc_token;
                bool setup_ready = false;
                {
                    const auto start = std::chrono::steady_clock::now();
                    std::optional<long long> ready_after_s;
                    for (;;) {
                        if (still_current && !still_current()) {
                            SAM_LOG_INFO("first-login reapply: superseded during setup "
                                         "wait; aborting");
                            return;
                        }
                        if (!cc_token)
                            cc_token = steam_local::read_connect_cache_value(login_lower);
                        setup_ready =
                            steam_local::login_config_presence(sid).userdata_present;
                        const auto elapsed = std::chrono::steady_clock::now() - start;
                        if (setup_ready && cc_token && !ready_after_s)
                            ready_after_s = std::chrono::duration_cast<std::chrono::seconds>(
                                elapsed).count();
                        if (elapsed >= kMinSetupDwell && setup_ready && cc_token) break;
                        if (elapsed >= kSetupTimeout) break;
                        std::this_thread::sleep_for(kSetupPollInterval);
                    }
                    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start).count();
                    if (setup_ready)
                        SAM_LOG_INFO("first-login reapply: setup ready after {}s, waited {}s; "
                                     "restarting", ready_after_s.value_or(0), waited);
                    else
                        SAM_LOG_WARN("first-login reapply: userdata not created within {}s; "
                                     "proceeding and writing login state explicitly", waited);
                }

                // Steam is now set up. Shut it down (flushes its now-full localconfig/
                // sharedconfig and creates remotecache.vdf), apply the settings in-place so
                // they survive, then relaunch with auto-login. One restart, first login only.
                LaunchResult tmp;
                if (!shutdown_running_steam(exe, tmp))
                    SAM_LOG_WARN("first-login reapply: shutdown: {}", tmp.message);
                // Let Steam's final config flush settle before we edit and relaunch.
                std::this_thread::sleep_for(std::chrono::milliseconds(800));

                if (defer_launch_options) {
                    const auto r = cs2_config::apply_launch_options(sid, opts);
                    if (!r.ok)
                        SAM_LOG_WARN("first-login reapply: launch options: {}", r.message);
                }
                if (defer_news) {
                    const auto r = steam_local::set_news_notify_off(sid);
                    if (!r.ok) SAM_LOG_WARN("first-login reapply: news: {}", r.message);
                }
                if (defer_cloud) {
                    // remotecache.vdf now exists, so the cloud write's remotecache refresh
                    // can outrank the server copy.
                    const auto r = steam_local::set_cloud_enabled_off(sid);
                    if (!r.ok) SAM_LOG_WARN("first-login reapply: cloud: {}", r.message);
                }

                // A newer launch may have started during the shutdown; don't relaunch the
                // wrong account or fight its auto-login. The prefs above are already on disk.
                if (still_current && !still_current()) {
                    SAM_LOG_INFO("first-login reapply: superseded; skipping relaunch");
                    return;
                }
                // Write the full login state ourselves so the relaunch auto-logs in like
                // the NFA token path, instead of trusting how far Steam's setup got before
                // we stopped it: the remember-me token (restored only if the shutdown
                // dropped it), the loginusers entry, and the config.vdf name -> SteamID map.
                // set_remembered_account is not enough here: it no-ops when Steam hasn't
                // written the loginusers entry yet, which is exactly the first-login race.
                if (cc_token && !steam_local::read_connect_cache_value(login_lower))
                    steam_local::write_connect_cache_raw(login_lower, *cc_token);
                steam_local::ensure_loginusers_entry(sid, login_lower, "");
                steam_local::ensure_config_vdf_account(sid, login_lower);
                platform::registry::set_auto_login_user(login_w);
                platform::registry::set_remember_password(true);
                auto relaunch_pid = platform::process::launch(exe, L"");
                if (!relaunch_pid) {
                    SAM_LOG_WARN("first-login reapply: relaunch failed; settings are on disk "
                                 "and apply on the next sign-in");
                    return;
                }
                SAM_LOG_INFO("first-login reapply: applied prefs and relaunched as {}",
                             login_lower);

                if (hwid_profile.has_value()) {
                    core::Account tmp_acc;
                    tmp_acc.hwid = hwid_profile;
                    tmp_acc.login = login_lower;
                    maybe_inject_hwid(tmp_acc, *relaunch_pid, hwid_mask);
                }

                // Resume the CS2 auto-launch the caller skipped for the deferred case:
                // Steam is back up and signing in on this final instance, so there's no
                // restart left to race it.
                if (method != core::LoginMethod::Normal) {
                    SAM_LOG_INFO("first-login reapply: starting CS2 autostart post-relaunch");
                    cs2_autostart::start_async(method, sid, loader, lum_loader);
                }
            };
    }

    out.guard_code_was_typed = login_driver::run_async(*pid, std::move(creds));

    SAM_LOG_INFO("launch: started steam.exe pid={} as login={}", *pid, a.login);
    return out;
}

}  // namespace sam::launch
