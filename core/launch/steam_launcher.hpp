#pragma once

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
LaunchResult launch_account(const core::Account& account);

}  // namespace sam::launch
