#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/trade/models.hpp"

namespace sam::core::trade {

struct InventoryFetchResult {
    bool ok = false;
    int http_status = 0;
    std::string error;
    bool needs_relogin = false;
    std::vector<InventoryItem> items;
    bool more = false;                 // Steam paginated; more pages remain
    std::uint64_t last_assetid = 0;    // cursor for the next page
};

// Fetches an inventory via steamcommunity.com/inventory/<owner>/<appid>/<contextid>
// using the web session cookie, joining assets with descriptions. For our own CS2
// inventory pass owner = a.steam_id_64, app_id 730, context_id 2. Mutates `a` with
// any refreshed session. Must run on a worker thread.
InventoryFetchResult fetch_inventory(core::Account& a, std::uint64_t owner_steam_id_64,
                                     std::uint32_t app_id = 730, std::uint32_t context_id = 2,
                                     int count = 2000, std::uint64_t start_assetid = 0);

}  // namespace sam::core::trade
