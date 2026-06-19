#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sam::cs2_gc {

class GcSession;

// Claims the chosen items from the weekly free-reward (care package) store. The ids
// must come from GcSession::personal_store().items and not exceed its balance.
bool redeem_free_reward(GcSession& gc, const std::vector<std::uint64_t>& item_ids,
                        std::string& error);

}  // namespace sam::cs2_gc
