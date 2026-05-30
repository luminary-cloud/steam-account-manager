#pragma once

#include <optional>
#include <string>

#include "core/account_store/account.hpp"

namespace sam::sda {

struct PhoneStatus {
    bool has_phone = false;
    std::string masked_number;
};

// Queries IPhone/QueryAccountPhoneStatus to find out if the account already has a
// verified phone. Required before AddAuthenticator.
std::optional<PhoneStatus> query_phone_status(const core::Account& account);

struct AddAuthenticatorResult {
    bool ok = false;
    int status_code = 0;
    std::string raw_response;            // JSON of the response, kept for debugging
    std::optional<core::SteamGuardAccount> sda;
};

// First half of Add Steam Guard: sends AddAuthenticator and returns the
// authenticator object that must be persisted before Finalize. Save what you
// get even if the user closes the wizard; otherwise their account is left in
// an unrecoverable state.
AddAuthenticatorResult add_authenticator(const core::Account& account);

struct FinalizeResult {
    bool ok = false;
    int status_code = 0;
    bool needs_retry = false;            // true on status 89 (bad sms code)
    bool needs_resync = false;           // true on status 88 (time desync)
    std::string error;
};

// Second half: confirms the SMS activation code. Retries up to 10 times on bad
// codes are handled by the caller.
FinalizeResult finalize_add(const core::Account& account, const std::string& sms_code);

struct RemoveResult {
    bool ok = false;
    int status_code = 0;
    std::string error;
};

// Removes the authenticator. `scheme = 1` reverts to email-based Steam Guard,
// `scheme = 2` strips Steam Guard entirely. Either way the 15-day market/trade
// hold applies.
RemoveResult remove_authenticator(const core::Account& account, int scheme = 1);

struct TwoFactorStatus {
    bool ok = false;               // HTTP 200 and the response parsed
    int status_code = 0;           // response.status, if Steam sends one
    int authenticator_type = 0;    // 0 none, 1 Valve mobile authenticator, 2 third-party
    int steamguard_scheme = 0;
    int state = 0;                 // raw authenticator state, kept for logging
    std::string token_gid;         // gid of the authenticator currently on the account
    std::string device_identifier;
    std::string raw_response;      // kept for debugging, mirrors AddAuthenticatorResult
    std::string error;
};

// Queries ITwoFactorService/QueryStatus to learn which authenticator (if any) is
// currently active on the account. Used to confirm that an imported maFile flagged
// not-finalized actually maps to a live authenticator before clearing fully_enrolled.
TwoFactorStatus query_two_factor_status(const core::Account& account);

}  // namespace sam::sda
