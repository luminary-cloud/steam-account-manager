#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/account_store/account.hpp"

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
};

// Launches Steam logged in as `account`:
//   1. close any running steam.exe (graceful -shutdown then hard-kill)
//   2. spawn the configured steam.exe
//   3. hand off to login_driver, which polls for the Chromium login window
//      and fills credentials / Remember / 2FA via UI Automation
//
// `LaunchResult::guard_code_was_typed` is true if the async driver was
// started (it does its work after launch_account returns).
//
// NFA accounts (account.is_nfa) are dispatched to launch_account_with_token,
// which injects the JWT instead of typing a password.
LaunchResult launch_account(const core::Account& account);

// Resolves the configured steam.exe path, or returns nullopt and fills `out`
// with a SteamNotInstalled reason. Shared by the password and token paths.
std::optional<std::filesystem::path> resolve_steam_exe(LaunchResult& out);

// Gracefully closes any running steam.exe (-shutdown, then hard kill). Returns
// false and fills `out` only if a hard kill fails. No-op (returns true) if Steam
// isn't running.
bool shutdown_running_steam(const std::filesystem::path& steam_exe, LaunchResult& out);

}  // namespace sam::launch
