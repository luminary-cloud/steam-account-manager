#include "core/steam_local/login_prefs.hpp"

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

// Backs up `path` (UTC timestamp suffix) then atomically writes `text`. restrict_acl
// stays false for Steam-owned files Steam must still read. On first login the file may
// not exist yet: there is nothing to back up, so just create the parent tree and write.
// Fills out.message on failure.
bool backup_and_write(const fs::path& path, const std::string& text, bool restrict_acl,
                      LoginPrefResult& out) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        fs::path bak = path;
        bak += L".bak." + timestamp_suffix();
        fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            out.message = "backup of " + path.filename().string() + " failed: " + ec.message();
            return false;
        }
    } else {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            out.message =
                "could not create " + path.parent_path().string() + ": " + ec.message();
            return false;
        }
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

}  // namespace

LoginPrefResult set_news_notify_off(std::uint64_t steam_id_64) {
    LoginPrefResult out;

    auto userdata = account_userdata_dir(steam_id_64, out);
    if (!userdata) return out;

    const fs::path path = *userdata / L"config" / L"localconfig.vdf";
    out.target = path;

    // On first login Steam hasn't written localconfig.vdf yet; start from an empty store
    // so the setting is in place before Steam creates the file during sign-in. When the
    // file already exists we parse and amend it (backup_and_write backs it up first).
    std::error_code ec;
    VdfNode root;
    if (fs::is_regular_file(path, ec)) {
        const std::string text = read_file_to_string(path);
        if (!text.empty()) root = parse_vdf(text);
    }

    // UserLocalConfigStore > news > NotifyAvailableGames
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

    // Add CloudEnabled=0 to sharedconfig.vdf (parse + amend if it exists, else start empty).
    // This file is cloud-synced (app 7): we deliberately leave remotecache.vdf UNTOUCHED so
    // Steam sees our edit as an ordinary pending local change and UPLOADS it on the next sync
    // (the first-login restart's relaunch), turning Cloud off server-side so it actually
    // sticks for accounts that already have cloud data. Rewriting remotecache to match the
    // edit + bumping its ChangeNumber instead masked the change and made Steam distrust the
    // local cache and re-download the server's "cloud on" copy. A brand-new account has no
    // server copy to fight, so the local write stands on its own either way.
    std::error_code ec;
    VdfNode root;
    if (fs::is_regular_file(shared, ec)) {
        const std::string text = read_file_to_string(shared);
        if (!text.empty()) root = parse_vdf(text);
    }

    // UserRoamingConfigStore > Software > Valve > Steam > CloudEnabled
    VdfNode& store = find_or_add_block(root, "UserRoamingConfigStore");
    VdfNode& software = find_or_add_block(store, "Software");
    VdfNode& valve = find_or_add_block(software, "Valve");
    VdfNode& steam_blk = find_or_add_block(valve, "Steam");
    upsert_scalar_ci(steam_blk, "CloudEnabled", "0");

    const std::string serialized = serialize_root(root);
    // Steam-owned cloud file: keep the inherited ACL so Steam's sync can still read it.
    if (!backup_and_write(shared, serialized, /*restrict_acl=*/false, out)) return out;

    out.ok = true;
    out.message = "Steam Cloud disabled";
    SAM_LOG_INFO("login prefs: set CloudEnabled=0 for userdata/{}",
                 static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull));
    return out;
}

LoginConfigPresence login_config_presence(std::uint64_t steam_id_64) {
    LoginConfigPresence out;
    LoginPrefResult ignored;
    auto userdata = account_userdata_dir(steam_id_64, ignored);
    if (!userdata) return out;  // unresolved SteamID / Steam not installed: both false

    std::error_code ec;
    out.userdata_present = fs::is_directory(*userdata, ec);
    out.localconfig_present =
        fs::is_regular_file(*userdata / L"config" / L"localconfig.vdf", ec);
    out.sharedconfig_present =
        fs::is_regular_file(*userdata / L"7" / L"remote" / L"sharedconfig.vdf", ec);
    return out;
}

}  // namespace sam::steam_local
