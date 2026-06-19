#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sam::steam_cm {

// Expands a CMsgMulti body into its individual sub-message frames. `payload` is
// CMsgMulti.message_body and `size_unzipped` is CMsgMulti.size_unzipped (0 means
// the payload is not compressed). Each output buffer is a complete CM frame ready
// for decode(). Returns false on a malformed payload.
bool expand_multi(const std::string& payload, std::uint32_t size_unzipped,
                  std::vector<std::vector<std::uint8_t>>& out);

}  // namespace sam::steam_cm
