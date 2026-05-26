#pragma once

#include <cstdint>
#include <string>

#include "core/crypto/secure_string.hpp"

namespace sam::launch::login_driver {

// Everything the worker needs, snapshotted out of the Account so the
// driver thread never reaches back into UI/vault state. SecureString
// destructors zero the underlying memory.
struct Credentials {
    crypto::SecureString login;
    crypto::SecureString password;
    std::string shared_secret;            // empty if no Steam Guard mobile auth
    bool remember_password = true;
};

// Spawns a detached worker that drives the freshly-launched Steam login UI
// via Windows UI Automation:
//   1. waits for the login window owned by `steam_pid` or one of its
//      steamwebhelper children
//   2. fills the credential form, toggles Remember if needed, clicks
//      Sign In
//   3. on a 2FA prompt, generates the TOTP from `creds.shared_secret`
//      and types each digit into the digit pad
//   4. waits for the main client window to confirm success
//
// A newer call supersedes any in-flight worker: the older worker checks a
// generation counter on each poll and exits when a fresher run has started.
// Always returns true and runs entirely async.
bool run_async(std::uint32_t steam_pid, Credentials creds);

}  // namespace sam::launch::login_driver
