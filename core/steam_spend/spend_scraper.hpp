#pragma once

#include "core/account_store/account.hpp"
#include "core/http/response.hpp"

namespace sam::steam_spend {

// Performs an authenticated GET of help.steampowered.com AccountSpend for
// `account`. Returns the raw HTML body. The HTTP status is included so callers
// can detect login redirects (a login page in the body means the session
// expired); session-expiry detection is content-based at the caller.
http::Response fetch_account_spend(const core::Account& account);

}  // namespace sam::steam_spend
