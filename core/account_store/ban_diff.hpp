#pragma once

#include <cstdint>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/account_store/ban_event.hpp"

namespace sam::core {

PreviousSnapshot snapshot_from(const BanStatus& bans, const CS2Status& cs2);

// Events that fire when prev -> (new_bans, new_cs2). prev.snapshot_unix == 0
// (first observation) returns empty.
std::vector<BanEvent> diff_account_state(const std::string& account_id,
                                         const PreviousSnapshot& prev,
                                         const BanStatus& new_bans,
                                         const CS2Status& new_cs2,
                                         std::int64_t now);

}  // namespace sam::core
