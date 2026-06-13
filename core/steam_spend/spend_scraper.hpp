#pragma once

#include "core/account_store/account.hpp"
#include "core/http/response.hpp"

namespace sam::steam_spend {

// Authenticated GET of help.steampowered.com AccountSpend for `account`.
// Session-expiry detection is content-based at the caller.
http::Response fetch_account_spend(const core::Account& account);

}  // namespace sam::steam_spend
