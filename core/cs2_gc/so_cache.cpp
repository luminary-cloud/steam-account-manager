#include "core/cs2_gc/so_cache.hpp"

#include <cstring>

#include "core/log.hpp"

namespace sam::cs2_gc {

namespace {

constexpr std::uint32_t kDefIndexStorageUnit = 1201;
constexpr std::uint32_t kAttrPaintIndex = 6;
constexpr std::uint32_t kAttrPaintSeed = 7;
constexpr std::uint32_t kAttrPaintWear = 8;
constexpr std::uint32_t kAttrKillEaterValue = 80;
constexpr std::uint32_t kAttrCustomName = 111;
constexpr std::uint32_t kAttrStickerId = 113;  // sticker slot 0 id (sticker/graffiti/patch kit)
constexpr std::uint32_t kAttrMusicId = 166;    // music kit id
constexpr std::uint32_t kAttrCasketItemCount = 270;
constexpr std::uint32_t kAttrCasketIdLow = 272;
constexpr std::uint32_t kAttrCasketIdHigh = 273;

const std::string* attr_bytes(const ::CSOEconItem& it, std::uint32_t def_index) {
    for (const auto& a : it.attribute())
        if (a.def_index() == def_index && a.has_value_bytes()) return &a.value_bytes();
    return nullptr;
}

std::uint32_t u32le(const std::string& b) {
    if (b.size() < 4) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(b.data());
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

float f32le(const std::string& b) {
    const std::uint32_t u = u32le(b);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

}  // namespace

EconItem parse_econ_item(const ::CSOEconItem& it) {
    EconItem e;
    e.id = it.id();
    e.def_index = it.def_index();
    e.quality = it.quality();
    e.rarity = it.rarity();
    e.origin = it.origin();
    e.flags = it.flags();
    e.inventory = it.inventory();
    e.level = it.level();
    e.in_use = it.in_use();
    const bool is_new = (e.inventory >> 30) & 1u;
    e.new_item = is_new;
    e.position = is_new ? 0 : (e.inventory & 0xffffu);

    if (!it.custom_name().empty()) e.custom_name = it.custom_name();

    const std::string* lo = attr_bytes(it, kAttrCasketIdLow);
    const std::string* hi = attr_bytes(it, kAttrCasketIdHigh);
    if (lo && hi)
        e.casket_id = static_cast<std::uint64_t>(u32le(*lo)) |
                      (static_cast<std::uint64_t>(u32le(*hi)) << 32);

    if (e.custom_name.empty())
        if (const std::string* nm = attr_bytes(it, kAttrCustomName); nm && nm->size() > 2)
            e.custom_name = nm->substr(2);

    if (const std::string* pi = attr_bytes(it, kAttrPaintIndex)) e.paint_index = static_cast<int>(f32le(*pi));
    if (const std::string* ps = attr_bytes(it, kAttrPaintSeed)) e.paint_seed = static_cast<int>(f32le(*ps));
    if (const std::string* pw = attr_bytes(it, kAttrPaintWear)) e.paint_wear = f32le(*pw);
    if (attr_bytes(it, kAttrKillEaterValue)) e.stattrak = true;
    // Sticker/music ids are raw u32 (unlike the float-encoded paint attrs above).
    if (const std::string* m = attr_bytes(it, kAttrMusicId)) e.music_id = static_cast<int>(u32le(*m));
    if (const std::string* s = attr_bytes(it, kAttrStickerId)) e.sticker_kit_id = static_cast<int>(u32le(*s));

    if (e.def_index == kDefIndexStorageUnit) {
        e.is_storage_unit = true;
        e.casket_item_count = 0;
        if (const std::string* c = attr_bytes(it, kAttrCasketItemCount))
            e.casket_item_count = static_cast<int>(u32le(*c));
    }

    // TEMP charm-discovery: log any attribute we don't decode yet so a charm-owning
    // account reveals the keychain attribute id and value. Remove once kAttrKeychainId is set.
    for (const auto& a : it.attribute()) {
        const std::uint32_t ad = a.def_index();
        const bool known = ad == kAttrPaintIndex || ad == kAttrPaintSeed || ad == kAttrPaintWear ||
                           ad == kAttrKillEaterValue || ad == kAttrCustomName || ad == kAttrStickerId ||
                           ad == kAttrMusicId || ad == kAttrCasketItemCount || ad == kAttrCasketIdLow ||
                           ad == kAttrCasketIdHigh;
        if (!known && a.has_value_bytes())
            SAM_LOG_INFO("gc: def_index={} undecoded attr {}={} ({} bytes)", e.def_index, ad,
                         u32le(a.value_bytes()), a.value_bytes().size());
    }
    return e;
}

}  // namespace sam::cs2_gc
