#pragma once

#include <cstdint>
#include <string>

#include "core/trade/models.hpp"

namespace sam::core::trade {

// Parses a Steam trade URL of the form
// https://steamcommunity.com/tradeoffer/new/?partner=<accountid>&token=<token>
// Returns ok=false when the partner id or token can't be extracted.
TradeUrl parse_trade_url(const std::string& url);

// SteamID individual-account conversions. The trade URL and the WebAPI's
// accountid_other use the 32-bit account id; community POST bodies and the
// inventory path use the 64-bit SteamID.
std::uint64_t account_id_to_steam_id_64(std::uint32_t account_id);
std::uint32_t steam_id_64_to_account_id(std::uint64_t steam_id_64);

}  // namespace sam::core::trade
