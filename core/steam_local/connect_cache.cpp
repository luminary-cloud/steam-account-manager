#include "core/steam_local/connect_cache.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "core/crypto/secure_string.hpp"
#include "core/log.hpp"
#include "core/steam_local/crc32.hpp"
#include "core/steam_local/loginusers.hpp"   // VdfNode + tree helpers
#include "platform/dpapi.hpp"
#include "platform/fs.hpp"
#include "platform/paths.hpp"

namespace sam::steam_local {

namespace {

std::string to_lower_ascii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string to_hex_lower(std::span<const std::uint8_t> bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Walks (creating as needed) a chain of nested block keys, returning the
// deepest one. Used to reach Steam's deeply-nested ConnectCache block.
VdfNode* ensure_block_path(VdfNode& root, std::initializer_list<const char*> path) {
    VdfNode* cur = &root;
    for (const char* key : path) {
        VdfNode* child = find_child(*cur, key);
        if (!child) {
            VdfNode n;
            n.key = key;
            n.is_block = true;
            cur->children.push_back(std::move(n));
            child = find_child(*cur, key);
        }
        child->is_block = true;
        cur = child;
    }
    return cur;
}

}  // namespace

std::optional<std::filesystem::path> steam_local_vdf_path() {
    auto root = platform::local_appdata_root();
    if (root.empty()) return std::nullopt;
    return root / "Steam" / "local.vdf";
}

bool write_connect_cache_token(const std::string& account_name,
                               const crypto::SecureString& refresh_token) {
    if (account_name.empty() || refresh_token.empty()) return false;

    auto path_opt = steam_local_vdf_path();
    if (!path_opt) {
        SAM_LOG_WARN("connect_cache: %LocalAppData% could not be resolved");
        return false;
    }
    const auto path = *path_opt;

    const std::string name = to_lower_ascii(account_name);

    // ConnectCache entry key: hex(crc32(name)) + "1". Built as a string (not
    // (crc<<4)|1, which overflows uint32 when the top nibble is set).
    char hexbuf[16];
    std::snprintf(hexbuf, sizeof(hexbuf), "%x",
                  static_cast<unsigned>(crc32_ieee(name)));
    const std::string key = std::string(hexbuf) + "1";

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

    const std::string value = to_hex_lower(blob);

    // read_file returns "" if local.vdf is absent; we then build the tree fresh.
    VdfNode root = parse_vdf(read_file(path));
    VdfNode* connect_cache = ensure_block_path(
        root, {"MachineUserConfigStore", "Software", "Valve", "Steam", "ConnectCache"});
    upsert_scalar(*connect_cache, key, value);

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
                 name, key, value.size(), path.string());
    return true;
}

}  // namespace sam::steam_local
