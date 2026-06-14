#include "core/cs2_config/launch_options.hpp"

#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "core/log.hpp"
#include "core/steam_local/loginusers.hpp"
#include "platform/fs.hpp"
#include "platform/registry.hpp"

namespace sam::cs2_config {

namespace {

namespace fs = std::filesystem;
using steam_local::VdfNode;

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

}  // namespace

DeployResult apply_launch_options(std::uint64_t steam_id_64, const std::string& options) {
    DeployResult out;

    if (steam_id_64 == 0) {
        out.message = "account has no resolved SteamID";
        return out;
    }

    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return out;
    }

    // userdata folders are named by the SteamID3 account number (lower 32 bits).
    const auto account_id = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    const fs::path path = *steam / L"userdata" / std::to_wstring(account_id) /
                          L"config" / L"localconfig.vdf";
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

    VdfNode root = steam_local::parse_vdf(text);

    // UserLocalConfigStore > Software > Valve > Steam > Apps > 730 > LaunchOptions
    VdfNode& store = find_or_add_block(root, "UserLocalConfigStore");
    VdfNode& software = find_or_add_block(store, "Software");
    VdfNode& valve = find_or_add_block(software, "Valve");
    VdfNode& steam_blk = find_or_add_block(valve, "Steam");
    VdfNode& apps = find_or_add_block(steam_blk, "Apps");
    VdfNode& app730 = find_or_add_block(apps, "730");
    upsert_scalar_ci(app730, "LaunchOptions", options);

    std::string serialized;
    // localconfig.vdf wraps everything in UserLocalConfigStore; write each root
    // child at depth 0 (matches Steam's layout).
    for (const auto& c : root.children) steam_local::serialize_node(c, serialized, 0);

    fs::path bak = path;
    bak += L".bak." + timestamp_suffix();
    fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        out.message = "backup of existing localconfig.vdf failed: " + ec.message();
        return out;
    }

    try {
        platform::atomic_write_file(
            path, std::span<const std::uint8_t>(
                      reinterpret_cast<const std::uint8_t*>(serialized.data()),
                      serialized.size()));
    } catch (const std::exception& ex) {
        out.message = std::string("write failed: ") + ex.what();
        return out;
    }

    out.ok = true;
    out.message = "CS2 launch options applied";
    SAM_LOG_INFO("cs2 launch options: set appid 730 LaunchOptions for userdata/{}", account_id);
    return out;
}

}  // namespace sam::cs2_config
