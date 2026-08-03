#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "core/account_store/account.hpp"
#include "core/launch/hwid_inject.hpp"

namespace sam::launch {

enum class LaunchStatus {
    Ok,
    SteamNotInstalled,
    KillFailed,
    SpawnFailed,
};

struct LaunchResult {
    LaunchStatus status = LaunchStatus::Ok;
    std::string message;
    bool guard_code_was_typed = false;
    bool first_login_deferred = false;
    InjectOutcome hwid_outcome{};
    std::string hwid_error;
};

// Everything the launch is configured with, so the two sign-in paths and the first-login
// reapply share one description of the launch instead of re-listing a dozen arguments.
struct LaunchOptions {
    // Written to appid 730's LaunchOptions in the account's localconfig.vdf during the
    // Steam-down window (between shutdown and relaunch) so Steam can't overwrite it. Empty
    // leaves Steam's existing launch options alone.
    std::string cs2_launch_options;

    // Force Steam Cloud and the new-release news notification off for the account, in the
    // same Steam-down window. Never re-enabled when turned back off.
    bool disable_cloud_on_login = false;
    bool disable_news_on_login = false;

    // Sign the account in as Invisible, and turn its Remote Play off. Both are per-account
    // keys in localconfig.vdf, written in the same window.
    bool login_invisible = false;
    bool disable_remote_play_on_login = false;

    // Block the account's subscribed CS2 workshop maps from downloading (marks each one
    // disabled in the account's own subscription list); when unset they are restored.
    bool disable_workshop_on_login = false;

    // Controls the login window's "Remember me" checkbox for password logins (off => Steam
    // doesn't save the session). No effect on the token path, whose sign-in requires a
    // remembered session regardless.
    bool remember_password = true;

    // Passed through to cs2_autostart only when a first-login reapply is deferred for a
    // CS2-autostart account: the reapply callback launches CS2 (and injects the loader)
    // after its restart. Ignored otherwise; the caller drives autostart directly.
    std::filesystem::path gamesense_loader;
    std::filesystem::path luminary_loader;

    // Which hardware identifiers the injected spoofer replaces.
    std::uint32_t hwid_component_mask = 0x3FFu;

    // Forces the token-injection path for a password account, using its cm_refresh_token.
    // NFA accounts always take the token path regardless.
    bool use_token = false;

    // Vault safe mode: no spoofer DLL is injected and no external loader runs, whatever the
    // account or the settings say. Honored by both sign-in paths and carried into
    // first_login_reapply so the deferred restart can't re-inject.
    bool safe_mode = false;

    // Runs once Steam is fully down and before the pre-writes above, the only point in a launch
    // where Steam's config can be edited freely. The tracer cleaner hooks in here: cleaning
    // later would delete the token this launch just injected, cleaning earlier would be undone
    // by Steam's shutdown rewrite. Blocking. Null (the default) does nothing. Deliberately not
    // honored by first_login_reapply's restart, which shuts Steam down again after the sign-in.
    std::function<void()> after_steam_down;
};

// Launches Steam signed in as `account`: closes any running steam.exe, spawns the configured
// one, then hands off to login_driver to fill credentials and 2FA via UI Automation.
// `guard_code_was_typed` is true if the async driver was started; it works after this returns.
// NFA accounts go to launch_account_with_token, which injects the JWT instead.
//
// Steam initializes a first login from scratch and wipes the pre-writes above, so those
// settings are deferred to first_login_reapply, which restarts Steam once with them applied
// and sets `first_login_deferred`. Both sign-in methods defer.
LaunchResult launch_account(const core::Account& account, const LaunchOptions& opts);

// Resolves the configured steam.exe path, or returns nullopt and fills `out`
// with a SteamNotInstalled reason. Shared by the password and token paths.
std::optional<std::filesystem::path> resolve_steam_exe(LaunchResult& out);

// Gracefully closes any running steam.exe (-shutdown, then hard kill). Returns
// false and fills `out` only if a hard kill fails. No-op (returns true) if Steam
// isn't running.
bool shutdown_running_steam(const std::filesystem::path& steam_exe, LaunchResult& out);

}  // namespace sam::launch
