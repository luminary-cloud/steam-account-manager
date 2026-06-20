#include "core/steam_local/connect_cache.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "core/crypto/secure_string.hpp"
#include "core/log.hpp"
#include "core/steam_local/crc32.hpp"
#include "core/strings.hpp"
#include "core/steam_local/loginusers.hpp"
#include "platform/dpapi.hpp"
#include "platform/fs.hpp"
#include "platform/paths.hpp"

namespace sam::steam_local {

namespace {

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Key is the string hex(crc32(name)) + "1", not (crc<<4)|1 which overflows
// uint32 when the top nibble is set. `name` must already be lowercased.
std::string connect_cache_key(const std::string& name) {
    char hexbuf[16];
    std::snprintf(hexbuf, sizeof(hexbuf), "%x",
                  static_cast<unsigned>(crc32_ieee(name)));
    return std::string(hexbuf) + "1";
}

}  // namespace

std::optional<std::filesystem::path> steam_local_vdf_path() {
    auto root = platform::local_appdata_root();
    if (root.empty()) return std::nullopt;
    return root / "Steam" / "local.vdf";
}

bool write_connect_cache_raw(const std::string& account_name,
                             const std::string& raw_value) {
    if (account_name.empty() || raw_value.empty()) return false;

    auto path_opt = steam_local_vdf_path();
    if (!path_opt) {
        SAM_LOG_WARN("connect_cache: %LocalAppData% could not be resolved");
        return false;
    }
    const auto path = *path_opt;

    const std::string name = core::to_lower(account_name);
    const std::string key = connect_cache_key(name);

    VdfNode root = parse_vdf(read_file(path));  // "" when local.vdf is absent
    VdfNode* connect_cache = ensure_block_path(
        root, {"MachineUserConfigStore", "Software", "Valve", "Steam", "ConnectCache"});
    upsert_scalar(*connect_cache, key, raw_value);

    std::string serialized;
    for (const auto& c : root.children) serialize_node(c, serialized, 0);

    try {
        platform::atomic_write_file(path,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(serialized.data()),
                serialized.size()));
    } catch (const std::exception& ex) {
        SAM_LOG_ERROR("connect_cache: write failed: {}", ex.what());
        return false;
    }

    SAM_LOG_INFO("connect_cache: wrote token for '{}' (key {}, {} hex chars) to {}",
                 name, key, raw_value.size(), path.string());
    return true;
}

bool write_connect_cache_token(const std::string& account_name,
                               const crypto::SecureString& refresh_token) {
    if (account_name.empty() || refresh_token.empty()) return false;

    const std::string name = core::to_lower(account_name);

    // DPAPI-wrap the bare JWT, entropy = the account name bytes (no NUL).
    std::string plaintext(refresh_token.begin(), refresh_token.end());
    std::vector<std::uint8_t> blob;
    try {
        blob = platform::dpapi::protect(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(plaintext.data()),
                plaintext.size()),
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
    } catch (const std::exception& ex) {
        crypto::zero_buffer(plaintext.data(), plaintext.size());
        SAM_LOG_ERROR("connect_cache: DPAPI protect failed: {}", ex.what());
        return false;
    }
    crypto::zero_buffer(plaintext.data(), plaintext.size());

    return write_connect_cache_raw(account_name, core::to_hex_lower(blob));
}

std::optional<std::string> read_connect_cache_value(const std::string& account_name) {
    if (account_name.empty()) return std::nullopt;

    auto path_opt = steam_local_vdf_path();
    if (!path_opt) return std::nullopt;

    const std::string contents = read_file(*path_opt);
    if (contents.empty()) return std::nullopt;

    const std::string key = connect_cache_key(core::to_lower(account_name));

    VdfNode root = parse_vdf(contents);
    VdfNode* node = &root;
    for (const char* k : {"MachineUserConfigStore", "Software", "Valve", "Steam",
                          "ConnectCache"}) {
        node = find_child(*node, k);
        if (!node || !node->is_block) return std::nullopt;
    }
    VdfNode* entry = find_child(*node, key);
    if (!entry || entry->is_block || entry->value.empty()) return std::nullopt;
    return entry->value;
}

}  // namespace sam::steam_local
