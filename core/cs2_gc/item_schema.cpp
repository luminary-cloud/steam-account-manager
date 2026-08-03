#include "core/cs2_gc/item_schema.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/cs2_gc/kv.hpp"
#include "core/http/client.hpp"
#include "core/log.hpp"
#include "platform/fs.hpp"
#include "platform/paths.hpp"

namespace sam::cs2_gc {

namespace fs = std::filesystem;

namespace {

constexpr char kMirrorItemsGame[] =
    "https://raw.githubusercontent.com/SteamDatabase/GameTracking-CS2/master/"
    "game/csgo/pak01_dir/scripts/items/items_game.txt";
constexpr char kMirrorEnglish[] =
    "https://raw.githubusercontent.com/SteamDatabase/GameTracking-CS2/master/"
    "game/csgo/pak01_dir/resource/csgo_english.txt";

constexpr char kByMykelBase[] =
    "https://raw.githubusercontent.com/ByMykel/CSGO-API/main/public/api/en/";

constexpr std::uint32_t kDefaultAgentDef = 987;

std::string http_get(const std::string& url) {
    http::Request req;
    req.method = http::Method::Get;
    req.url = url;
    req.timeout_seconds = 60;
    req.connect_timeout_seconds = 10;
    const auto resp = http::request(req);
    if (resp.status == 200 && !resp.body.empty()) return resp.body;
    SAM_LOG_WARN("gc: schema fetch failed {} (status {})", url, resp.status);
    return {};
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_text(const fs::path& p, const std::string& s) {
    platform::atomic_write_file(
        p, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()),
        false);
}

void to_lower(std::string& s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

const char* wear_name(float w) {
    if (w < 0.0f) return nullptr;
    if (w < 0.07f) return "Factory New";
    if (w < 0.15f) return "Minimal Wear";
    if (w < 0.38f) return "Field-Tested";
    if (w < 0.45f) return "Well-Worn";
    return "Battle-Scarred";
}

std::uint32_t parse_hex_rgb(const std::string& s) {
    std::size_t i = (!s.empty() && s.front() == '#') ? 1 : 0;
    std::uint32_t v = 0;
    int n = 0;
    for (; i < s.size() && n < 6; ++i) {
        const char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | static_cast<std::uint32_t>(d);
        ++n;
    }
    return v;
}

}  // namespace

bool ItemSchema::load(std::uint32_t version, const std::string& gc_items_game_url) {
    const fs::path dir = platform::cache_dir() / "cs2_schema";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path items_path = dir / "items_game.txt";
    const fs::path english_path = dir / "csgo_english.txt";
    const fs::path ver_path = dir / "schema.ver";

    const bool have_core = fs::exists(items_path) && fs::exists(english_path);
    const std::uint32_t cached_ver =
        static_cast<std::uint32_t>(std::strtoul(read_text(ver_path).c_str(), nullptr, 10));
    const bool need_fetch = !have_core || (version != 0 && cached_ver != version);

    std::string items_txt = have_core ? read_text(items_path) : std::string{};
    std::string english_txt = have_core ? read_text(english_path) : std::string{};

    if (need_fetch) {
        const std::string url = gc_items_game_url.empty() ? std::string(kMirrorItemsGame)
                                                          : gc_items_game_url;
        std::string ig = http_get(url);
        if (ig.empty() && url != kMirrorItemsGame) ig = http_get(kMirrorItemsGame);
        std::string en = http_get(kMirrorEnglish);

        if (!ig.empty()) {
            items_txt = std::move(ig);
            write_text(items_path, items_txt);
        }
        if (!en.empty()) {
            english_txt = std::move(en);
            write_text(english_path, english_txt);
        }
        if (!items_txt.empty() && !english_txt.empty() && version != 0)
            write_text(ver_path, std::to_string(version));
    }

    KvNode items_root;
    if (!items_txt.empty() && parse_kv(items_txt, items_root)) {
        if (const KvNode* ig = items_root.find("items_game")) {
            std::unordered_map<std::string, std::string> prefab_token;
            if (const KvNode* prefabs = ig->find("prefabs"))
                for (const auto& pre : prefabs->children)
                    if (const KvNode* n = pre.find("item_name")) prefab_token[pre.key] = n->value;

            if (const KvNode* items = ig->find("items"))
                for (const auto& it : items->children) {
                    if (it.key.empty() || !std::isdigit(static_cast<unsigned char>(it.key[0]))) continue;
                    const auto def = static_cast<std::uint32_t>(std::strtoul(it.key.c_str(), nullptr, 10));
                    std::string token;
                    if (const KvNode* n = it.find("item_name")) token = n->value;
                    if (token.empty())
                        if (const KvNode* pf = it.find("prefab")) {
                            const std::string& list = pf->value;
                            std::size_t s = 0;
                            while (s <= list.size()) {
                                const std::size_t e = list.find(' ', s);
                                const std::string name =
                                    list.substr(s, e == std::string::npos ? std::string::npos : e - s);
                                if (const auto pit = prefab_token.find(name); pit != prefab_token.end()) {
                                    token = pit->second;
                                    break;
                                }
                                if (e == std::string::npos) break;
                                s = e + 1;
                            }
                        }
                    if (!token.empty()) def_name_token_[def] = std::move(token);
                }

            if (const KvNode* kits = ig->find("paint_kits"))
                for (const auto& kit : kits->children) {
                    if (kit.key.empty() || !std::isdigit(static_cast<unsigned char>(kit.key[0]))) continue;
                    const int id = std::atoi(kit.key.c_str());
                    if (const KvNode* tag = kit.find("description_tag")) paint_tag_[id] = tag->value;
                }
        }
    }

    if (!english_txt.empty()) {
        KvNode eng_root;
        if (parse_kv(english_txt, eng_root))
            if (const KvNode* lang = eng_root.find("lang"))
                if (const KvNode* tokens = lang->find("Tokens"))
                    for (const auto& t : tokens->children) {
                        std::string k = t.key;
                        to_lower(k);
                        english_[k] = t.value;
                    }
    }

    struct Src {
        const char* file;
        IconKind kind;
    };
    static constexpr Src kSrcs[] = {
        {"skins.json", IconKind::Skin},
        {"crates.json", IconKind::Crate},
        {"agents.json", IconKind::Agent},
        {"collectibles.json", IconKind::Collectible},
        {"stickers.json", IconKind::StickerKit},
        {"graffiti.json", IconKind::StickerKit},
        {"patches.json", IconKind::StickerKit},
        {"music_kits.json", IconKind::Music},
        {"tools.json", IconKind::Tool},
        {"keychains.json", IconKind::Keychain},
    };
    for (const auto& src : kSrcs) {
        const fs::path p = dir / src.file;
        std::string body = fs::exists(p) ? read_text(p) : std::string{};
        if (body.empty() || need_fetch) {
            std::string fresh = http_get(std::string(kByMykelBase) + src.file);
            if (!fresh.empty()) {
                body = std::move(fresh);
                write_text(p, body);
            }
        }
        if (!body.empty()) parse_bymykel(body, src.kind);
    }
    non_storable_defs_.insert(kDefaultAgentDef);

    loaded_ = !def_name_token_.empty() || !by_defpaint_.empty();
    SAM_LOG_INFO(
        "gc: item schema loaded ({} items, {} skins/cases, {} stickers, {} music, {} keychains, {} strings)",
        def_name_token_.size(), by_defpaint_.size(), by_sticker_kit_.size(), by_music_.size(),
        by_keychain_.size(), english_.size());
    return loaded_;
}

void ItemSchema::parse_bymykel(const std::string& body, IconKind kind) {
    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(body);
    } catch (const std::exception&) {
        return;
    }
    if (!arr.is_array()) return;
    for (const auto& e : arr) {
        try {
            Entry entry{e.value("name", std::string{}), e.value("image", std::string{})};
            if (entry.name.empty() && entry.icon.empty()) continue;
            if (e.contains("rarity") && e["rarity"].is_object() && e["rarity"].contains("color") &&
                e["rarity"]["color"].is_string())
                entry.rarity_rgb = parse_hex_rgb(e["rarity"]["color"].get<std::string>());
            if (kind == IconKind::Skin) {
                const long long def = e.at("weapon").at("weapon_id").get<long long>();
                std::string paint = "0";
                if (e.contains("paint_index") && e["paint_index"].is_string())
                    paint = e["paint_index"].get<std::string>();
                by_defpaint_[std::to_string(def) + ":" + paint] = std::move(entry);
            } else if (e.contains("def_index") && e["def_index"].is_string()) {
                const std::string ds = e["def_index"].get<std::string>();
                switch (kind) {
                    case IconKind::Crate:
                    case IconKind::Agent:
                    case IconKind::Tool:
                        by_defpaint_[ds + ":0"] = std::move(entry);
                        break;
                    case IconKind::Collectible:
                        non_storable_defs_.insert(static_cast<std::uint32_t>(std::stoul(ds)));
                        by_defpaint_[ds + ":0"] = std::move(entry);
                        break;
                    case IconKind::StickerKit:
                        by_sticker_kit_[std::stoi(ds)] = std::move(entry);
                        break;
                    case IconKind::Music:
                        by_music_[std::stoi(ds)] = std::move(entry);
                        break;
                    case IconKind::Keychain:
                        by_keychain_[std::stoi(ds)] = std::move(entry);
                        break;
                    case IconKind::Skin:
                        break;
                }
            }
        } catch (const std::exception&) {
            continue;
        }
    }
}

std::string ItemSchema::resolve_token(const std::string& token) const {
    if (token.empty()) return {};
    std::string key = token[0] == '#' ? token.substr(1) : token;
    to_lower(key);
    if (const auto it = english_.find(key); it != english_.end()) return it->second;
    return token[0] == '#' ? token.substr(1) : token;
}

std::string ItemSchema::fallback_name(const EconItem& item) const {
    const auto it = def_name_token_.find(item.def_index);
    if (it == def_name_token_.end()) return {};
    std::string weapon = resolve_token(it->second);
    if (weapon.empty()) return {};
    std::string label = weapon;
    if (item.paint_index > 0)
        if (const auto p = paint_tag_.find(item.paint_index); p != paint_tag_.end()) {
            const std::string skin = resolve_token(p->second);
            if (!skin.empty()) label += " | " + skin;
        }
    return label;
}

std::string ItemSchema::decorate(const std::string& base_in, const EconItem& item) const {
    const std::string base = base_in.empty() ? ("Item #" + std::to_string(item.def_index)) : base_in;
    std::string label;
    if (item.stattrak) label += "StatTrak™ ";
    label += base;
    if (const char* wear = wear_name(item.paint_wear)) {
        label += " (";
        label += wear;
        label += ")";
    }
    if (!item.custom_name.empty()) label += "  \"" + item.custom_name + "\"";
    return label;
}

ResolvedItem ItemSchema::resolve(const EconItem& item) const {
    const int paint = item.paint_index > 0 ? item.paint_index : 0;
    const Entry* e = nullptr;
    if (const auto it = by_defpaint_.find(std::to_string(item.def_index) + ":" + std::to_string(paint));
        it != by_defpaint_.end())
        e = &it->second;
    if (e == nullptr && item.music_id >= 0)
        if (const auto it = by_music_.find(item.music_id); it != by_music_.end()) e = &it->second;
    if (e == nullptr && item.sticker_kit_id >= 0)
        if (const auto it = by_sticker_kit_.find(item.sticker_kit_id); it != by_sticker_kit_.end())
            e = &it->second;
    if (e == nullptr && item.keychain_id >= 0)
        if (const auto it = by_keychain_.find(item.keychain_id); it != by_keychain_.end())
            e = &it->second;

    ResolvedItem r;
    r.icon_url = e != nullptr ? e->icon : std::string{};
    r.name = decorate(e != nullptr ? e->name : fallback_name(item), item);
    if (e != nullptr) r.border_rgb = e->rarity_rgb;

    constexpr std::uint32_t kFlagNotTradable = 8;
    r.storable = non_storable_defs_.find(item.def_index) == non_storable_defs_.end() &&
                 item.origin != 0 && (item.flags & kFlagNotTradable) == 0;
    return r;
}

std::shared_ptr<const ItemSchema> ItemSchema::shared(std::uint32_t version,
                                                     const std::string& gc_items_game_url) {
    static std::mutex mtx;
    static std::unordered_map<std::uint32_t, std::shared_ptr<const ItemSchema>> cache;
    std::lock_guard lk(mtx);
    if (const auto it = cache.find(version); it != cache.end()) return it->second;
    auto schema = std::make_shared<ItemSchema>();
    schema->load(version, gc_items_game_url);
    if (schema->loaded()) cache.emplace(version, schema);
    return schema;
}

}  // namespace sam::cs2_gc
