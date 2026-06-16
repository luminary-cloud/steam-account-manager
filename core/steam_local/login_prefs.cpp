#include "core/steam_local/login_prefs.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include <mbedtls/md.h>

#include "core/log.hpp"
#include "core/steam_local/loginusers.hpp"
#include "platform/fs.hpp"
#include "platform/registry.hpp"

namespace sam::steam_local {

namespace {

namespace fs = std::filesystem;

std::wstring timestamp_suffix() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    std::wstringstream ss;
    ss << std::put_time(&tm, L"%Y%m%dT%H%M%SZ");
    return ss.str();
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Steam's casing for these keys varies across versions, so match case-insensitively
// but create with the canonical casing if absent.
VdfNode& find_or_add_block(VdfNode& parent, std::string_view key) {
    for (auto& c : parent.children) {
        if (c.is_block && iequal(c.key, key)) return c;
    }
    VdfNode node;
    node.key = std::string(key);
    node.is_block = true;
    parent.children.push_back(std::move(node));
    return parent.children.back();
}

void upsert_scalar_ci(VdfNode& parent, std::string_view key, std::string_view value) {
    for (auto& c : parent.children) {
        if (!c.is_block && iequal(c.key, key)) {
            c.value = std::string(value);
            return;
        }
    }
    VdfNode node;
    node.key = std::string(key);
    node.value = std::string(value);
    parent.children.push_back(std::move(node));
}

std::string read_file_to_string(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Steam wraps each VDF file in a single top-level store; serialize_node writes each
// root child at depth 0 to match Steam's layout.
std::string serialize_root(const VdfNode& root) {
    std::string out;
    for (const auto& c : root.children) serialize_node(c, out, 0);
    return out;
}

std::span<const std::uint8_t> as_bytes(const std::string& s) {
    return std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// Lowercase hex SHA-1 of the buffer, as Steam stores in remotecache.vdf.
std::string sha1_hex(const std::string& data) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (info == nullptr) return {};
    std::array<unsigned char, 20> digest{};
    if (mbedtls_md(info, reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                   digest.data()) != 0) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (unsigned char b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::int64_t now_unix() { return static_cast<std::int64_t>(std::time(nullptr)); }

// Backs up `path` (UTC timestamp suffix) then atomically writes `text`. restrict_acl
// stays false for Steam-owned files Steam must still read. Fills out.message on failure.
bool backup_and_write(const fs::path& path, const std::string& text, bool restrict_acl,
                      LoginPrefResult& out) {
    std::error_code ec;
    fs::path bak = path;
    bak += L".bak." + timestamp_suffix();
    fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        out.message = "backup of " + path.filename().string() + " failed: " + ec.message();
        return false;
    }
    try {
        platform::atomic_write_file(path, as_bytes(text), restrict_acl);
    } catch (const std::exception& ex) {
        out.message = std::string("write failed: ") + ex.what();
        return false;
    }
    return true;
}

// Resolves <Steam>/userdata/<accountid> for the account. Fills out.message and returns
// nullopt if the account has no resolved SteamID or Steam isn't installed.
std::optional<fs::path> account_userdata_dir(std::uint64_t steam_id_64, LoginPrefResult& out) {
    if (steam_id_64 == 0) {
        out.message = "account has no resolved SteamID";
        return std::nullopt;
    }
    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return std::nullopt;
    }
    // userdata folders are named by the SteamID3 account number (lower 32 bits).
    const auto account_id = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    return *steam / L"userdata" / std::to_wstring(account_id);
}

// Refreshes the sharedconfig.vdf entry in <userdata>/7/remotecache.vdf to match the
// freshly-written file (size/sha/time) and bumps the appid ChangeNumber, so Steam's
// cloud client treats the local copy as the newer one to upload. Best-effort: a missing
// cache or entry just leaves the sharedconfig write to stand on its own.
void update_remotecache(const fs::path& userdata, const std::string& shared_text) {
    const fs::path cache = userdata / L"7" / L"remotecache.vdf";
    std::error_code ec;
    if (!fs::is_regular_file(cache, ec)) {
        SAM_LOG_WARN("login prefs: remotecache.vdf missing; skipped cloud sync metadata");
        return;
    }
    const std::string text = read_file_to_string(cache);
    if (text.empty()) {
        SAM_LOG_WARN("login prefs: remotecache.vdf unreadable; skipped cloud sync metadata");
        return;
    }

    VdfNode root = parse_vdf(text);
    VdfNode* appblk = find_child(root, "7");
    if (appblk == nullptr) {
        SAM_LOG_WARN("login prefs: remotecache.vdf has no appid 7 block; skipped");
        return;
    }
    VdfNode* file = find_child(*appblk, "sharedconfig.vdf");
    if (file == nullptr) {
        SAM_LOG_WARN("login prefs: remotecache.vdf has no sharedconfig.vdf entry; skipped");
        return;
    }

    const std::string now = std::to_string(now_unix());
    upsert_scalar(*file, "size", std::to_string(shared_text.size()));
    const std::string sha = sha1_hex(shared_text);
    if (!sha.empty()) upsert_scalar(*file, "sha", sha);
    upsert_scalar(*file, "localtime", now);
    upsert_scalar(*file, "time", now);

    // Bump ChangeNumber so the local copy outranks the cloud copy on next sync.
    if (VdfNode* cn = find_child(*appblk, "ChangeNumber")) {
        std::int64_t n = 0;
        try {
            n = std::stoll(cn->value);
        } catch (...) {
            n = 0;
        }
        cn->value = std::to_string(n + 1);
    }

    LoginPrefResult ignored;
    if (!backup_and_write(cache, serialize_root(root), /*restrict_acl=*/false, ignored)) {
        SAM_LOG_WARN("login prefs: remotecache.vdf update failed: {}", ignored.message);
    }
}

}  // namespace

LoginPrefResult set_news_notify_off(std::uint64_t steam_id_64) {
    LoginPrefResult out;

    auto userdata = account_userdata_dir(steam_id_64, out);
    if (!userdata) return out;

    const fs::path path = *userdata / L"config" / L"localconfig.vdf";
    out.target = path;

    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        out.message = "localconfig.vdf not found; sign into the account in Steam once first";
        return out;
    }

    const std::string text = read_file_to_string(path);
    if (text.empty()) {
        out.message = "localconfig.vdf is empty or unreadable";
        return out;
    }

    // UserLocalConfigStore > news > NotifyAvailableGames
    VdfNode root = parse_vdf(text);
    VdfNode& store = find_or_add_block(root, "UserLocalConfigStore");
    VdfNode& news = find_or_add_block(store, "news");
    upsert_scalar_ci(news, "NotifyAvailableGames", "0");

    // localconfig.vdf is read by the main Steam client as the same user, so the default
    // restricted ACL is fine here (matches cs2_config::apply_launch_options).
    if (!backup_and_write(path, serialize_root(root), /*restrict_acl=*/true, out)) return out;

    out.ok = true;
    out.message = "news notifications disabled";
    SAM_LOG_INFO("login prefs: set news NotifyAvailableGames=0 for userdata/{}",
                 static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull));
    return out;
}

LoginPrefResult set_cloud_enabled_off(std::uint64_t steam_id_64) {
    LoginPrefResult out;

    auto userdata = account_userdata_dir(steam_id_64, out);
    if (!userdata) return out;

    const fs::path shared = *userdata / L"7" / L"remote" / L"sharedconfig.vdf";
    out.target = shared;

    std::error_code ec;
    if (!fs::is_regular_file(shared, ec)) {
        out.message = "sharedconfig.vdf not found; sign into the account in Steam once first";
        return out;
    }

    const std::string text = read_file_to_string(shared);
    if (text.empty()) {
        out.message = "sharedconfig.vdf is empty or unreadable";
        return out;
    }

    // UserRoamingConfigStore > Software > Valve > Steam > CloudEnabled
    VdfNode root = parse_vdf(text);
    VdfNode& store = find_or_add_block(root, "UserRoamingConfigStore");
    VdfNode& software = find_or_add_block(store, "Software");
    VdfNode& valve = find_or_add_block(software, "Valve");
    VdfNode& steam_blk = find_or_add_block(valve, "Steam");
    upsert_scalar_ci(steam_blk, "CloudEnabled", "0");

    const std::string serialized = serialize_root(root);
    // Steam-owned cloud file: keep the inherited ACL so Steam's sync can still read it.
    if (!backup_and_write(shared, serialized, /*restrict_acl=*/false, out)) return out;

    update_remotecache(*userdata, serialized);

    out.ok = true;
    out.message = "Steam Cloud disabled";
    SAM_LOG_INFO("login prefs: set CloudEnabled=0 for userdata/{}",
                 static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull));
    return out;
}

}  // namespace sam::steam_local
