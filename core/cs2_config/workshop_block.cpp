#include "core/cs2_config/workshop_block.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

// Steam's casing for these keys varies across versions, so match case-insensitively.
VdfNode* find_block_ci(VdfNode& parent, std::string_view key) {
    for (auto& c : parent.children) {
        if (c.is_block && iequal(c.key, key)) return &c;
    }
    return nullptr;
}

VdfNode* find_scalar_ci(VdfNode& parent, std::string_view key) {
    for (auto& c : parent.children) {
        if (!c.is_block && iequal(c.key, key)) return &c;
    }
    return nullptr;
}

void upsert_scalar_ci(VdfNode& parent, std::string_view key, std::string_view value) {
    if (VdfNode* s = find_scalar_ci(parent, key)) {
        s->value = std::string(value);
        return;
    }
    VdfNode node;
    node.key = std::string(key);
    node.value = std::string(value);
    parent.children.push_back(std::move(node));
}

// Removes every direct block child of `parent` whose key case-insensitively matches
// any id in `keys`.
void erase_block_children(VdfNode& parent, const std::set<std::string>& keys) {
    parent.children.erase(
        std::remove_if(parent.children.begin(), parent.children.end(),
                       [&](const VdfNode& c) {
                           if (!c.is_block) return false;
                           return keys.count(c.key) != 0;
                       }),
        parent.children.end());
}

std::string read_file_to_string(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string serialize_root(const VdfNode& root) {
    std::string out;
    for (const auto& c : root.children) steam_local::serialize_node(c, out, 0);
    return out;
}

std::span<const std::uint8_t> as_bytes(const std::string& s) {
    return std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// Splits a `subscribedby` scalar ("a,b,c") into its non-empty account-id tokens.
std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t comma = s.find(',', start);
        std::string_view tok =
            s.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                            : comma - start);
        std::string t = trim(tok);
        if (!t.empty()) out.push_back(std::move(t));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

std::optional<fs::path> workshop_acf_path(DeployResult& out) {
    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return std::nullopt;
    }
    return *steam / L"steamapps" / L"workshop" / L"appworkshop_730.acf";
}

// Item ids that already have content on disk under workshop/content/730; these are left
// untouched (already installed, so no download is pending for them).
std::set<std::string> installed_item_ids(const fs::path& steam) {
    std::set<std::string> ids;
    std::error_code ec;
    const fs::path content = steam / L"steamapps" / L"workshop" / L"content" / L"730";
    if (!fs::is_directory(content, ec)) return ids;
    for (auto it = fs::directory_iterator(content, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) ids.insert(it->path().filename().string());
    }
    return ids;
}

}  // namespace

DeployResult apply_workshop_block(std::uint64_t steam_id_64) {
    DeployResult out;

    if (steam_id_64 == 0) {
        out.message = "account has no resolved SteamID";
        return out;
    }
    auto acf = workshop_acf_path(out);
    if (!acf) return out;
    out.target = *acf;

    std::error_code ec;
    if (!fs::is_regular_file(*acf, ec)) {
        // No workshop bookkeeping for CS2 yet: nothing subscribed to download.
        out.ok = true;
        out.message = "no CS2 workshop acf; nothing to block";
        return out;
    }

    // A prior run may have locked this read-only; clear it so we can back up + rewrite.
    platform::set_file_read_only(*acf, false);

    const std::string text = read_file_to_string(*acf);
    if (text.empty()) {
        out.message = "appworkshop_730.acf is empty or unreadable";
        return out;
    }

    VdfNode root = steam_local::parse_vdf(text);
    VdfNode* app = find_block_ci(root, "AppWorkshop");
    if (!app) {
        // Unexpected shape; leave it alone rather than risk corrupting Steam's file.
        out.ok = true;
        out.message = "appworkshop_730.acf has no AppWorkshop block; left unchanged";
        return out;
    }

    const auto steam_dir = acf->parent_path().parent_path().parent_path();  // <Steam>
    const std::set<std::string> installed = installed_item_ids(steam_dir);
    const std::string id3 =
        std::to_string(static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull));

    VdfNode* details = find_block_ci(*app, "WorkshopItemDetails");
    VdfNode* items = find_block_ci(*app, "WorkshopItemsInstalled");

    std::set<std::string> remove_ids;  // items to drop entirely (only this account wanted)
    int rewritten = 0;                 // items where we only dropped this account

    if (details) {
        for (auto& item : details->children) {
            if (!item.is_block) continue;
            if (installed.count(item.key)) continue;  // already on disk: leave it
            VdfNode* sub = find_scalar_ci(item, "subscribedby");
            if (!sub) continue;
            std::vector<std::string> subs = split_csv(sub->value);
            const auto before = subs.size();
            subs.erase(std::remove(subs.begin(), subs.end(), id3), subs.end());
            if (subs.size() == before) continue;  // this account wasn't a subscriber
            if (subs.empty()) {
                remove_ids.insert(item.key);
            } else {
                std::string joined;
                for (std::size_t i = 0; i < subs.size(); ++i) {
                    if (i) joined += ',';
                    joined += subs[i];
                }
                sub->value = joined;
                ++rewritten;
            }
        }
    }

    if (!remove_ids.empty()) {
        if (details) erase_block_children(*details, remove_ids);
        if (items) erase_block_children(*items, remove_ids);
    }

    // Nothing pending is being fetched now; drop the top-level download flag too. The
    // read-only lock below keeps it 0 across Steam's post-login reconcile.
    upsert_scalar_ci(*app, "NeedsDownload", "0");

    if (remove_ids.empty() && rewritten == 0) {
        // Account subscribes to nothing new; still lock so Steam can't queue anything.
        SAM_LOG_INFO("workshop block: no pending items for account {}; locking acf", id3);
    }

    // Back up, then atomically write. restrict_acl stays false: Steam must still READ the
    // acf, and locking its DACL would strip the app-package ACEs its sandbox needs.
    fs::path bak = *acf;
    bak += L".bak." + timestamp_suffix();
    fs::copy_file(*acf, bak, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        out.message = "backup of appworkshop_730.acf failed: " + ec.message();
        return out;
    }
    try {
        platform::atomic_write_file(*acf, as_bytes(serialize_root(root)),
                                    /*restrict_acl=*/false);
    } catch (const std::exception& ex) {
        out.message = std::string("write failed: ") + ex.what();
        return out;
    }
    // Lock read-only so Steam's post-login reconcile can't re-add this account's subs or
    // flip NeedsDownload back. Steam can still read a read-only file, so sign-in is fine.
    platform::set_file_read_only(*acf, true);

    out.ok = true;
    out.message = "CS2 workshop downloads blocked (" +
                  std::to_string(remove_ids.size()) + " removed, " +
                  std::to_string(rewritten) + " kept for other accounts)";
    SAM_LOG_INFO("workshop block: account {} -> removed {} item(s), retained {} shared, "
                 "locked appworkshop_730.acf",
                 id3, remove_ids.size(), rewritten);
    return out;
}

DeployResult unlock_workshop_block() {
    DeployResult out;
    auto acf = workshop_acf_path(out);
    if (!acf) return out;
    out.target = *acf;

    std::error_code ec;
    if (!fs::is_regular_file(*acf, ec)) {
        out.ok = true;
        out.message = "no CS2 workshop acf; nothing to unlock";
        return out;
    }
    platform::set_file_read_only(*acf, false);
    out.ok = true;
    out.message = "CS2 workshop downloads re-enabled";
    SAM_LOG_INFO("workshop block: cleared read-only lock on appworkshop_730.acf");
    return out;
}

}  // namespace sam::cs2_config
