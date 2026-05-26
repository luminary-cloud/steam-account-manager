#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/steam_api/web_api.hpp"

namespace sam::steam_api {

struct OwnedGamesInfo {
    int games_count = 0;
    std::int64_t total_playtime_minutes = 0;
};

std::unordered_map<std::uint64_t, core::WebProfile> fetch_summaries(
    const WebApiConfig& cfg,
    const std::vector<std::uint64_t>& steam_ids);

std::optional<int> fetch_steam_level(const WebApiConfig& cfg, std::uint64_t steam_id);

std::optional<OwnedGamesInfo> fetch_owned_games(const WebApiConfig& cfg, std::uint64_t steam_id);

}  // namespace sam::steam_api
