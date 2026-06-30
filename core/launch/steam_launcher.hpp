#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

// Launches Steam logged in as `account`: closes any running steam.exe, spawns the
// configured one, then hands off to login_driver to fill credentials / 2FA via UI
// Automation. `guard_code_was_typed` is true if the async driver was started (it does
// its work after this returns). NFA accounts are dispatched to
// launch_account_with_token, which injects the JWT instead of typing a password.
//
// `cs2_launch_options`, when non-empty, is written to appid 730's LaunchOptions in
// the account's localconfig.vdf during the Steam-down window (between shutdown and
// relaunch) so Steam can't overwrite it.
//
// `disable_cloud_on_login` / `disable_news_on_login`, when set, force Steam Cloud and
// the new-release news notification off for the account in the same Steam-down window.
//
// `remember_password` controls the login window's "Remember me" checkbox for password
// logins (off => Steam doesn't save the session). It has no effect on NFA accounts, whose
// token sign-in requires a remembered session regardless.
//
// `gamesense_loader` is passed through to cs2_autostart only when a first-login reapply
// is deferred for a CS2-autostart account: the reapply callback launches CS2 (and injects
// gamesense) after its restart. Ignored otherwise; the caller drives autostart directly.
//
// `use_token`, when true, forces the token-injection path (launch_account_with_token) even
// for password accounts, using their cm_refresh_token. NFA accounts always use the token
// path regardless of this flag.
LaunchResult launch_account(const core::Account& account,
                            std::string_view cs2_launch_options = {},
                            bool disable_cloud_on_login = false,
                            bool disable_news_on_login = false,
                            bool remember_password = true,
                            std::filesystem::path gamesense_loader = {},
                            std::uint32_t hwid_component_mask = 0x3FFu,
                            bool use_token = false);

// Resolves the configured steam.exe path, or returns nullopt and fills `out`
// with a SteamNotInstalled reason. Shared by the password and token paths.
std::optional<std::filesystem::path> resolve_steam_exe(LaunchResult& out);

// Gracefully closes any running steam.exe (-shutdown, then hard kill). Returns
// false and fills `out` only if a hard kill fails. No-op (returns true) if Steam
// isn't running.
bool shutdown_running_steam(const std::filesystem::path& steam_exe, LaunchResult& out);

}  // namespace sam::launch
