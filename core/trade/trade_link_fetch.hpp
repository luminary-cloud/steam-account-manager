#pragma once

#include <string>

#include "core/account_store/account.hpp"

namespace sam::core::trade {

// Scrapes the account's trade-offer privacy page and returns the full trade URL
// (https://steamcommunity.com/tradeoffer/new/?partner=..&token=..). Returns empty
// on failure (no session, network error, or token not found); sets *err with a
// short reason when given. Requires a web session (session_id + steam_login_secure
// on the account). Makes a blocking request, so run it on a worker thread.
std::string fetch_trade_link(const core::Account& a, std::string* err = nullptr);

}  // namespace sam::core::trade
