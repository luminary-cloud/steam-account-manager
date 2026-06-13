#pragma once

#include <cstdint>
#include <string>

#include "core/trade/models.hpp"

namespace sam::core::trade {

// Parses https://steamcommunity.com/tradeoffer/new/?partner=<accountid>&token=<token>.
// Returns ok=false when the partner id or token can't be extracted.
TradeUrl parse_trade_url(const std::string& url);

// 32-bit account id (trade URL, WebAPI accountid_other) to 64-bit individual
// SteamID (community POST bodies, inventory path).
std::uint64_t account_id_to_steam_id_64(std::uint32_t account_id);

}  // namespace sam::core::trade
