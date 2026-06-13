#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sam::core::trade {

// Mirrors Steam's ETradeOfferState. Values are wire-stable and come straight
// from the trade_offer_state field of IEconService/GetTradeOffers.
enum class TradeOfferState : int {
    Invalid = 1,
    Active = 2,
    Accepted = 3,
    Countered = 4,
    Expired = 5,
    Canceled = 6,
    Declined = 7,
    InvalidItems = 8,
    CreatedNeedsConfirmation = 9,
    CanceledBySecondFactor = 10,
    InEscrow = 11,
};

inline const char* to_string(TradeOfferState state) {
    switch (state) {
        case TradeOfferState::Invalid:                  return "Invalid";
        case TradeOfferState::Active:                   return "Active";
        case TradeOfferState::Accepted:                 return "Accepted";
        case TradeOfferState::Countered:                return "Countered";
        case TradeOfferState::Expired:                  return "Expired";
        case TradeOfferState::Canceled:                 return "Canceled";
        case TradeOfferState::Declined:                 return "Declined";
        case TradeOfferState::InvalidItems:             return "Invalid items";
        case TradeOfferState::CreatedNeedsConfirmation: return "Needs confirmation";
        case TradeOfferState::CanceledBySecondFactor:   return "Canceled (2FA)";
        case TradeOfferState::InEscrow:                 return "In escrow";
    }
    return "Unknown";
}

// One item, after joining an `assets` entry with its `descriptions` entry (keyed by
// classid+instanceid). For CS2: app_id 730, context_id 2.
struct InventoryItem {
    std::uint64_t asset_id = 0;
    std::uint64_t class_id = 0;
    std::uint64_t instance_id = 0;
    std::uint32_t app_id = 730;
    std::uint32_t context_id = 2;
    std::uint32_t amount = 1;
    std::string market_hash_name;
    std::string name;
    std::string type;
    std::string icon_url;    // partial path; prefix with the economy CDN base
    std::string name_color;  // hex without '#', e.g. "D2D2D2"
    bool tradable = false;
    bool marketable = false;
};

// A reference to one asset to put into a trade; the send payload only needs these.
struct TradeAssetRef {
    std::uint32_t app_id = 730;
    std::uint32_t context_id = 2;
    std::uint64_t asset_id = 0;
    std::uint32_t amount = 1;
};

struct TradeOffer {
    std::string offer_id;                  // tradeofferid
    std::uint32_t partner_account_id = 0;  // accountid_other (32-bit)
    std::uint64_t partner_steam_id_64 = 0;
    bool is_our_offer = false;
    TradeOfferState state = TradeOfferState::Invalid;
    std::string message;
    std::int64_t expiration_unix = 0;
    std::int64_t created_unix = 0;
    std::int64_t updated_unix = 0;
    std::int64_t escrow_end_unix = 0;  // > 0 means a trade hold is in effect
    bool needs_confirmation = false;   // state == CreatedNeedsConfirmation
    std::vector<InventoryItem> items_to_give;
    std::vector<InventoryItem> items_to_receive;
};

// Parsed trade URL: steamcommunity.com/tradeoffer/new/?partner=<id>&token=<tok>.
// partner_account_id is the 32-bit account id, NOT the 64-bit SteamID.
struct TradeUrl {
    std::uint32_t partner_account_id = 0;
    std::string token;
    bool ok = false;
};

}  // namespace sam::core::trade
