#pragma once

#include <cstdint>
#include <string>

#include "core/steam_auth/gen/base_gcmessages.pb.h"

namespace sam::cs2_gc {

// A flattened CS2 inventory item decoded from a CSOEconItem, with the attribute
// blobs we care about already unpacked.
struct EconItem {
    std::uint64_t id = 0;
    std::uint32_t def_index = 0;
    std::uint32_t quality = 0;
    std::uint32_t rarity = 0;
    std::uint32_t origin = 0;
    std::uint32_t flags = 0;
    std::uint32_t inventory = 0;
    std::uint32_t position = 0;
    std::uint32_t level = 0;
    bool in_use = false;
    bool new_item = false;  // inventory bit 30: freshly acquired / unacknowledged
    std::string custom_name;
    std::uint64_t casket_id = 0;     // non-zero when the item is stored in a unit
    int paint_index = -1;
    int paint_seed = -1;
    float paint_wear = -1.0f;
    int music_id = -1;        // music kit id (attr 166); -1 if not a music kit
    int sticker_kit_id = -1;  // sticker/graffiti/patch kit id (attr 113, slot 0); -1 if none
    int keychain_id = -1;     // charm/keychain def id; -1 if none
    bool stattrak = false;
    bool is_storage_unit = false;
    int casket_item_count = -1;      // count of stored items, storage units only
};

EconItem parse_econ_item(const ::CSOEconItem& proto);

}  // namespace sam::cs2_gc
