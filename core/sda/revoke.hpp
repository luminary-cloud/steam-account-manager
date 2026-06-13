#pragma once

#include <optional>
#include <string>

#include "core/account_store/account.hpp"

namespace sam::sda {

struct AddAuthenticatorResult {
    bool ok = false;
    int status_code = 0;
    std::string raw_response;
    std::optional<core::SteamGuardAccount> sda;
};

// Persist the returned authenticator before Finalize even if the user bails;
// otherwise the account is left in an unrecoverable state.
AddAuthenticatorResult add_authenticator(const core::Account& account);

struct FinalizeResult {
    bool ok = false;
    int status_code = 0;
    bool needs_retry = false;  // status 89: bad SMS code
    std::string error;
};

FinalizeResult finalize_add(const core::Account& account, const std::string& sms_code);

struct RemoveResult {
    bool ok = false;
    int status_code = 0;
    std::string error;
};

// scheme 1 reverts to email Steam Guard, scheme 2 strips it entirely; both
// trigger the 15-day market/trade hold.
RemoveResult remove_authenticator(const core::Account& account, int scheme = 1);

struct TwoFactorStatus {
    bool ok = false;
    int status_code = 0;
    int authenticator_type = 0;  // 0 none, 1 Valve mobile, 2 third-party
    int steamguard_scheme = 0;
    int state = 0;
    std::string token_gid;
    std::string device_identifier;
    std::string raw_response;
    std::string error;
};

TwoFactorStatus query_two_factor_status(const core::Account& account);

}  // namespace sam::sda
