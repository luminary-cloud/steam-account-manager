#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/cs2_gc/so_cache.hpp"

namespace sam::cs2_gc {

// A fully resolved inventory item ready for display.
struct ResolvedItem {
    std::string name;            // e.g. "StatTrak™ AK-47 | Redline (Field-Tested)"
    std::string icon_url;        // full Steam CDN image URL, empty when unknown
    bool storable = true;        // false for medals/coins/badges and base/default loadout items
    std::uint32_t border_rgb = 0;  // rarity/grade color packed 0xRRGGBB, 0 = neutral
};

// Resolves CS2 item names, icons, and storability. Names + the weapon/paint fallback
// come from the game schema (items_game + csgo_english); names + icons for the actual
// market items come from ByMykel/CSGO-API, keyed by def_index (+ paint_index for
// skins, or the kit-id attribute for stickers/graffiti/music kits). Files are fetched
// once and cached on disk.
class ItemSchema {
public:
    // Process-wide parsed schema for `version`, built once and reused across reconnects
    // and accounts. Never null; resolve() degrades gracefully when data is missing.
    static std::shared_ptr<const ItemSchema> shared(std::uint32_t version,
                                                    const std::string& gc_items_game_url);

    bool load(std::uint32_t version, const std::string& gc_items_game_url);
    bool loaded() const { return loaded_; }

    ResolvedItem resolve(const EconItem& item) const;

private:
    enum class IconKind { Skin, Crate, Agent, Collectible, StickerKit, Music, Tool, Keychain };

    struct Entry {
        std::string name;
        std::string icon;
        std::uint32_t rarity_rgb = 0;  // ByMykel rarity.color, packed 0xRRGGBB
    };

    std::string resolve_token(const std::string& token) const;
    // ByMykel/CSGO-API JSON array -> the maps below, per item kind.
    void parse_bymykel(const std::string& json, IconKind kind);
    // items_game weapon|skin name when ByMykel has no entry; "" if unknown.
    std::string fallback_name(const EconItem& item) const;
    // Adds the per-instance StatTrak™ prefix, wear suffix and nametag to a base name.
    std::string decorate(const std::string& base, const EconItem& item) const;

    bool loaded_ = false;
    std::unordered_map<std::uint32_t, std::string> def_name_token_;  // def_index -> item_name token
    std::unordered_map<int, std::string> paint_tag_;                 // paint_kit -> description_tag token
    std::unordered_map<std::string, std::string> english_;          // lowercased token -> text

    std::unordered_map<std::string, Entry> by_defpaint_;  // "<def>:<paint>" (skins/crates/agents/collectibles)
    std::unordered_map<int, Entry> by_sticker_kit_;       // sticker/graffiti/patch kit id
    std::unordered_map<int, Entry> by_music_;             // music kit id
    std::unordered_map<int, Entry> by_keychain_;          // charm/keychain def id
    std::unordered_set<std::uint32_t> non_storable_defs_;  // collectibles + base/default loadout defs
};

}  // namespace sam::cs2_gc
