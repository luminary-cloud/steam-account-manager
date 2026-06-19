#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::steam_login {

enum class GuardKind {
    None = 0,
    DeviceCode = 1,         // TOTP from the Mobile app / our shared_secret
    EmailCode = 2,
    DeviceConfirmation = 3, // approve in the Mobile app
    EmailConfirmation = 4,  // click link in email
};

struct BeginSessionResult {
    bool ok = false;
    std::string error;
    std::string client_id;
    std::string request_id;
    std::int64_t interval_seconds = 5;
    std::vector<GuardKind> allowed_confirmations;
    std::uint64_t steam_id = 0;
    std::int64_t weak_token_unix = 0;
};

struct PollResult {
    bool ok = false;
    std::string error;
    bool finished = false;
    crypto::SecureString access_token;
    crypto::SecureString refresh_token;
    std::string new_client_id;  // server sometimes rotates this
    bool needs_guard_input = false;
};

struct MobileLogin {
    std::string username;
    crypto::SecureString password;
    std::string device_friendly_name = "steam-account-manager";
    std::string platform_user_agent;
    bool steam_client = false;  // request the SteamClient audience (CS2 GC) not Mobile
};

BeginSessionResult begin_session(const MobileLogin& login);

bool submit_guard_code(const std::string& client_id,
                        std::uint64_t steam_id,
                        const std::string& code,
                        GuardKind kind,
                        std::string* error_out = nullptr);

PollResult poll_session(const std::string& client_id, const std::string& request_id);

// `on_guard_needed` returns the code to submit (empty to fail).
struct FullLoginResult {
    bool ok = false;
    std::string error;
    core::Account account;
};

FullLoginResult run_full_login(
    const MobileLogin& login,
    const std::function<std::string(const std::vector<GuardKind>&)>& on_guard_needed,
    const std::function<void(const std::string&)>& on_status);

// Acquires only a SteamClient-audience refresh token (for the CS2 Game Coordinator)
// via a fresh credentials login. Forces the SteamClient platform. on_guard_needed
// supplies the Steam Guard code (empty to fail).
struct ClientTokenResult {
    bool ok = false;
    std::string error;
    crypto::SecureString refresh_token;
    std::int64_t expires = 0;
    std::uint64_t steam_id = 0;
};

ClientTokenResult acquire_client_token(
    const MobileLogin& login,
    const std::function<std::string(const std::vector<GuardKind>&)>& on_guard_needed,
    const std::function<void(const std::string&)>& on_status);

// TOTP from the shared_secret when the server accepts DeviceCode, else `on_prompt`.
// The TOTP path needs time_aligner synced: codes off an unsynced clock are
// rejected as k_EResult_TwoFactorCodeMismatch (88).
std::string default_guard_provider(
    const std::optional<core::SteamGuardAccount>& sda,
    const std::vector<GuardKind>& allowed,
    const std::function<std::string(const std::vector<GuardKind>&)>& on_prompt);

}  // namespace sam::steam_login
