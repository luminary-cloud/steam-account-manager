#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/cs2_gc/so_cache.hpp"

namespace sam::cs2_gc {

class GcSession;

// Loads a storage unit's contents into the session inventory and returns them.
bool load_unit(GcSession& gc, std::uint64_t unit_id, std::vector<EconItem>& out,
               std::string& error);

// Moves a main-inventory item into a storage unit. Reports the unit-full case.
bool move_in(GcSession& gc, std::uint64_t unit_id, std::uint64_t item_id, std::string& error);

// Pulls a stored item back out into the main inventory. Reports the inventory-full case.
bool move_out(GcSession& gc, std::uint64_t unit_id, std::uint64_t item_id, std::string& error);

}  // namespace sam::cs2_gc
