#pragma once

#include <string>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/trade/models.hpp"

namespace sam::core::trade {

struct TradeOffersResult {
    bool ok = false;
    int http_status = 0;
    std::string error;
    bool needs_relogin = false;
    std::vector<TradeOffer> sent;
    std::vector<TradeOffer> received;
};

// Fetches trade offers via IEconService/GetTradeOffers using the account's
// access_token (no WebAPI key needed). Refreshes the access token first when near
// expiry, mutating `a` so the caller can persist it. active_only restricts to offers
// awaiting action. Must run on a worker thread.
TradeOffersResult get_trade_offers(core::Account& a, bool active_only = true);

}  // namespace sam::core::trade
