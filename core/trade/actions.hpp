#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/trade/models.hpp"

namespace sam::core::trade {

struct OfferActionResult {
    bool ok = false;
    int http_status = 0;
    std::string error;
    bool needs_confirmation = false;  // accept created a pending mobile confirmation
    bool needs_relogin = false;
};

struct SendOfferResult {
    bool ok = false;
    int http_status = 0;
    std::string error;
    std::string offer_id;             // tradeofferid on success
    bool needs_confirmation = false;  // Steam queued a mobile confirmation
    bool needs_relogin = false;
};

struct ConfirmResult {
    bool ok = false;
    bool not_found = false;  // the matching trade confirmation isn't visible yet
    std::string error;
};

// Accepts an incoming offer. partner_steam_id_64 is the offer's sender.
// needs_confirmation is set when accepting also gives items and Steam queued a
// mobile confirmation. Must run on a worker thread.
OfferActionResult accept_trade_offer(core::Account& a, const std::string& offer_id,
                                     std::uint64_t partner_steam_id_64);

// Declines an incoming offer.
OfferActionResult decline_trade_offer(core::Account& a, const std::string& offer_id);

// Cancels an outgoing offer we sent.
OfferActionResult cancel_trade_offer(core::Account& a, const std::string& offer_id);

// Sends a give-only trade offer of `give` to the partner named by the trade URL.
// Mutates `a` with any refreshed session. Must run on a worker thread.
SendOfferResult send_trade_offer(core::Account& a, const TradeUrl& trade_url,
                                 const std::vector<TradeAssetRef>& give,
                                 const std::string& message);

// One attempt to auto-resolve the mobile confirmation for a sent offer: fetches
// the confirmation list and approves the one whose creator_id matches offer_id.
// Sets not_found when no matching confirmation is visible yet (Steam can lag a
// few seconds after the send) so the caller can retry. Must run on a worker thread.
ConfirmResult confirm_sent_offer(core::Account& a, const std::string& offer_id);

}  // namespace sam::core::trade
