#include "core/steam_local/vdf_file.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <system_error>

#include "platform/fs.hpp"

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

std::span<const std::uint8_t> as_bytes(const std::string& s) {
    return std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

}  // namespace

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string serialize_root(const VdfNode& root) {
    std::string out;
    for (const auto& c : root.children) serialize_node(c, out, 0);
    return out;
}

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

VdfNode& find_or_add_block(VdfNode& parent, std::string_view key) {
    if (VdfNode* found = find_block_ci(parent, key)) return *found;
    VdfNode node;
    node.key = std::string(key);
    node.is_block = true;
    parent.children.push_back(std::move(node));
    return parent.children.back();
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

bool backup_and_write(const fs::path& path, const std::string& text, bool restrict_acl,
                      bool lock_read_only, std::string& error) {
    std::error_code ec;
    const bool existed = fs::is_regular_file(path, ec);
    if (existed) {

        platform::set_file_read_only(path, false);
        fs::path bak = path;
        bak += L".bak." + timestamp_suffix();
        fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "backup of " + path.filename().string() + " failed: " + ec.message();
            return false;
        }
    } else {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "could not create " + path.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    try {
        platform::atomic_write_file(path, as_bytes(text), restrict_acl);
    } catch (const std::exception& ex) {
        error = std::string("write failed: ") + ex.what();
        return false;
    }
    if (lock_read_only && existed) platform::set_file_read_only(path, true);
    return true;
}

}  // namespace sam::steam_local
