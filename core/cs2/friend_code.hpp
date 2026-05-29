#pragma once

#include <cstdint>
#include <string>

namespace sam::cs2 {

// Converts a SteamID64 to its CS2 friend code, e.g. 76561197960287930 -> "SUCVS-FADA".
// Returns "" when steam_id_64 == 0 or the MD5 step fails. Does not throw.
std::string friend_code(std::uint64_t steam_id_64);

}  // namespace sam::cs2
