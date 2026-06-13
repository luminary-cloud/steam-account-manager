#pragma once

#include <cstdint>
#include <string>

#include "core/crypto/secure_string.hpp"

namespace sam::launch::login_driver {

// Snapshotted out of the Account so the driver thread never reaches back into
// UI/vault state. SecureString destructors zero the underlying memory.
struct Credentials {
    crypto::SecureString login;
    crypto::SecureString password;
    std::string shared_secret;            // empty if no Steam Guard mobile auth
    bool remember_password = true;
    // Account id (low 32 bits of the SteamID) used to confirm a successful
    // sign-in via the registry ActiveUser value. 0 if unknown, in which case the
    // driver falls back to "any non-zero ActiveUser after credentials submitted".
    std::uint32_t expected_account_id = 0;
};

// Spawns a detached worker that drives the freshly-launched Steam login UI via
// Windows UI Automation: fills credentials, handles the Remember toggle, and types
// the 2FA digits, then waits for the main client window to confirm success. A newer
// call supersedes any in-flight worker via a generation counter checked on each poll.
// Always returns true and runs entirely async.
bool run_async(std::uint32_t steam_pid, Credentials creds);

}  // namespace sam::launch::login_driver
