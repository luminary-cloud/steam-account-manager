#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/steam_api/web_api.hpp"

namespace sam::steam_api {

// Looks up GetPlayerBans for up to 100 SteamIDs in one request.
// Returns one BanStatus per resolved id; missing ids will be absent from the map.
std::unordered_map<std::uint64_t, core::BanStatus> fetch_bans(
    const WebApiConfig& cfg,
    const std::vector<std::uint64_t>& steam_ids);

}  // namespace sam::steam_api
