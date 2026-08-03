#include "core/cs2_config/workshop_block.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "core/log.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/steam_local/vdf_file.hpp"
#include "platform/fs.hpp"
#include "platform/registry.hpp"

namespace sam::cs2_config {

namespace {

namespace fs = std::filesystem;
using steam_local::VdfNode;

std::optional<fs::path> subscriptions_path(std::uint64_t steam_id_64, DeployResult& out) {
    if (steam_id_64 == 0) {
        out.message = "account has no resolved SteamID";
        return std::nullopt;
    }
    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return std::nullopt;
    }

    const auto account_id = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    return *steam / L"userdata" / std::to_wstring(account_id) / L"ugc" /
           L"730_subscriptions.vdf";
}

void clear_legacy_acf_lock() {
    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) return;
    const fs::path acf = *steam / L"steamapps" / L"workshop" / L"appworkshop_730.acf";
    std::error_code ec;
    if (!fs::is_regular_file(acf, ec)) return;
    if (platform::set_file_read_only(acf, false))
        SAM_LOG_INFO("workshop block: cleared a legacy read-only lock on {}", acf.string());
}

int set_disabled_locally(const fs::path& path, bool disabled, DeployResult& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return 0;

    const std::string text = steam_local::read_text_file(path);
    if (text.empty()) {
        out.message = path.filename().string() + " is empty or unreadable";
        return -1;
    }

    VdfNode root = steam_local::parse_vdf(text);
    VdfNode* subscribed = nullptr;
    for (auto& top : root.children) {
        if (!top.is_block) continue;
        if ((subscribed = steam_local::find_block_ci(top, "subscribedfiles")) != nullptr) break;
    }
    if (subscribed == nullptr) {

        out.message = path.filename().string() + " has no subscribedfiles block; left unchanged";
        return 0;
    }

    const char* want = disabled ? "1" : "0";
    int changed = 0;
    for (auto& item : subscribed->children) {
        if (!item.is_block) continue;
        if (steam_local::find_scalar_ci(item, "publishedfileid") == nullptr) continue;
        VdfNode* flag = steam_local::find_scalar_ci(item, "disabled_locally");
        if (flag != nullptr && flag->value == want) continue;
        steam_local::upsert_scalar_ci(item, "disabled_locally", want);
        ++changed;
    }
    if (changed == 0) return 0;

    if (!steam_local::backup_and_write(path, steam_local::serialize_root(root),
                                       false, false,
                                       out.message)) {
        return -1;
    }
    return changed;
}

DeployResult set_block(std::uint64_t steam_id_64, bool disabled) {
    DeployResult out;
    clear_legacy_acf_lock();

    auto path = subscriptions_path(steam_id_64, out);
    if (!path) return out;
    out.target = *path;

    const int changed = set_disabled_locally(*path, disabled, out);
    if (changed < 0) return out;

    out.ok = true;
    const auto id3 = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    if (disabled) {
        out.message = "CS2 workshop downloads blocked (" + std::to_string(changed) +
                      " subscription(s) disabled)";
        SAM_LOG_INFO("workshop block: account {} -> disabled {} subscription(s)", id3, changed);
    } else {
        out.message = "CS2 workshop downloads re-enabled (" + std::to_string(changed) +
                      " subscription(s) restored)";
        SAM_LOG_INFO("workshop block: account {} -> restored {} subscription(s)", id3, changed);
    }
    return out;
}

}  // namespace

DeployResult apply_workshop_block(std::uint64_t steam_id_64) {
    return set_block(steam_id_64, true);
}

DeployResult clear_workshop_block(std::uint64_t steam_id_64) {
    return set_block(steam_id_64, false);
}

DeployResult clear_all_workshop_blocks() {
    DeployResult out;
    clear_legacy_acf_lock();

    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return out;
    }
    const fs::path userdata = *steam / L"userdata";
    out.target = userdata;

    std::error_code ec;
    int accounts = 0;
    int restored = 0;
    for (auto it = fs::directory_iterator(userdata, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const fs::path path = it->path() / L"ugc" / L"730_subscriptions.vdf";
        if (!fs::is_regular_file(path, ec)) continue;
        DeployResult one;
        const int changed = set_disabled_locally(path, false, one);
        if (changed < 0) {
            SAM_LOG_WARN("workshop block: {} -> {}", path.string(), one.message);
            continue;
        }
        ++accounts;
        restored += changed;
    }

    out.ok = true;
    out.message = "CS2 workshop downloads re-enabled (" + std::to_string(restored) +
                  " subscription(s) across " + std::to_string(accounts) + " account(s))";
    SAM_LOG_INFO("workshop block: cleared every block -> {} subscription(s) across {} account(s)",
                 restored, accounts);
    return out;
}

}  // namespace sam::cs2_config
