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

}  // namespace sam::sda
