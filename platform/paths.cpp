#include "platform/paths.hpp"

#include <array>
#include <cstdint>
#include <cwctype>
#include <span>
#include <string>
#include <system_error>

#include <windows.h>
#include <shlobj.h>

#include "platform/fs.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace sam::platform {

namespace {

constexpr const wchar_t* kRegSubkey       = L"Software\\steam-account-manager";
constexpr const wchar_t* kRegDataDir      = L"DataDir";
constexpr const wchar_t* kRegCleanupOld   = L"CleanupOld";
constexpr const wchar_t* kRegPendingVault = L"PendingVault";

std::string g_active_vault_id;

std::optional<std::filesystem::path> g_unavailable_custom;

std::filesystem::path module_directory() {
    std::array<wchar_t, MAX_PATH> buf{};
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0) return {};
    std::filesystem::path p(buf.data(), buf.data() + len);
    return p.parent_path();
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) {
        return {};
    }
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    return p;
}

std::wstring norm_lower(const std::filesystem::path& p) {
    std::wstring s = p.lexically_normal().wstring();
    if (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool paths_equal(const std::filesystem::path& a, const std::filesystem::path& b) {
    return norm_lower(a) == norm_lower(b);
}

bool is_subpath(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const std::wstring c = norm_lower(child);
    const std::wstring p = norm_lower(parent);
    if (p.empty() || c.size() < p.size()) return false;
    if (c.compare(0, p.size(), p) != 0) return false;
    return c.size() == p.size() || c[p.size()] == L'\\' || c[p.size()] == L'/';
}

std::filesystem::path read_registry_path(const wchar_t* value_name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubkey, 0, KEY_QUERY_VALUE, &key)
            != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0, size = 0;
    LONG rc = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
        size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(key, value_name, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return {};
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    if (buf.empty()) return {};
    std::filesystem::path p(buf);
    if (!p.is_absolute()) return {};
    return p;
}

bool write_registry_path(const wchar_t* value_name, const std::filesystem::path& dir) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubkey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring w = dir.wstring();
    const DWORD bytes = static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t));
    const LONG rc = RegSetValueExW(key, value_name, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(w.c_str()), bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool delete_registry_value(const wchar_t* value_name) {
    HKEY key = nullptr;
    const LONG open = RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubkey, 0, KEY_SET_VALUE, &key);
    if (open == ERROR_FILE_NOT_FOUND) return true;
    if (open != ERROR_SUCCESS) return false;
    const LONG rc = RegDeleteValueW(key, value_name);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

std::wstring read_registry_string(const wchar_t* value_name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubkey, 0, KEY_QUERY_VALUE, &key)
            != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0, size = 0;
    LONG rc = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
        size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(key, value_name, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return {};
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

bool write_registry_string(const wchar_t* value_name, const std::wstring& val) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubkey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t));
    const LONG rc = RegSetValueExW(key, value_name, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(val.c_str()), bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

}  // namespace

std::filesystem::path local_appdata_dir() {
    auto base = known_folder(FOLDERID_LocalAppData);
    if (base.empty()) {
        return module_directory() / "data";
    }
    return base / L"steam-account-manager";
}

std::filesystem::path local_appdata_root() {
    return known_folder(FOLDERID_LocalAppData);
}

std::filesystem::path default_data_dir() {
    auto exe = module_directory();
    if (!exe.empty() && std::filesystem::exists(exe / L"portable.flag")) {
        return exe / L"data";
    }
    return local_appdata_dir();
}

namespace {

std::filesystem::path resolve_data_dir() {
    auto exe = module_directory();
    if (!exe.empty() && std::filesystem::exists(exe / L"portable.flag")) {
        return exe / L"data";
    }
    const auto custom = read_registry_path(kRegDataDir);
    if (!custom.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(custom, ec)) {
            return custom;
        }
        std::filesystem::create_directories(custom, ec);
        if (!ec && std::filesystem::is_directory(custom, ec)) {
            return custom;
        }
        g_unavailable_custom = custom;
    }
    return local_appdata_dir();
}

}  // namespace

std::filesystem::path data_dir() {
    static const std::filesystem::path cached = resolve_data_dir();
    return cached;
}

bool using_custom_data_dir() {
    return !paths_equal(data_dir(), default_data_dir());
}

std::optional<std::filesystem::path> custom_data_dir_unavailable() {
    data_dir();
    return g_unavailable_custom;
}

bool set_custom_data_dir(const std::filesystem::path& dir, std::string* err) {
    if (dir.empty() || !dir.is_absolute()) {
        if (err) *err = "Choose an absolute folder.";
        return false;
    }
    if (!write_registry_path(kRegDataDir, dir)) {
        if (err) *err = "Couldn't save the data location to the registry.";
        return false;
    }
    return true;
}

bool clear_custom_data_dir(std::string* err) {
    if (!delete_registry_value(kRegDataDir)) {
        if (err) *err = "Couldn't clear the data location from the registry.";
        return false;
    }
    return true;
}

bool relocate_data_dir(const std::filesystem::path& new_dir, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (new_dir.empty() || !new_dir.is_absolute()) {
        return fail("Choose an absolute folder.");
    }
    const auto current = data_dir();
    if (paths_equal(current, new_dir)) {
        return fail("That's already the current data folder.");
    }
    if (is_subpath(new_dir, current)) {
        return fail("Choose a folder outside the current data folder.");
    }

    std::error_code ec;
    std::filesystem::create_directories(new_dir, ec);
    if (ec) return fail("Can't create the target folder: " + ec.message());

    try {
        const auto probe = new_dir / L".sam_write_test";
        const std::uint8_t b = 0;
        atomic_write_file(probe, std::span<const std::uint8_t>(&b, 1));
        std::filesystem::remove(probe, ec);
    } catch (const std::exception& e) {
        return fail(std::string("Target folder isn't writable: ") + e.what());
    }

    sweep_browser_cache();

    ec.clear();
    std::filesystem::copy(current, new_dir,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing |
        std::filesystem::copy_options::copy_symlinks, ec);
    if (ec) return fail("Copy failed: " + ec.message());

    write_registry_path(kRegCleanupOld, current);

    if (!set_custom_data_dir(new_dir, err)) {
        delete_registry_value(kRegCleanupOld);
        return false;
    }
    return true;
}

void cleanup_relocated_old_dir() {
    const auto old = read_registry_path(kRegCleanupOld);
    if (old.empty()) return;
    std::error_code ec;
    if (!paths_equal(old, data_dir())) {
        std::filesystem::remove_all(old, ec);
        if (std::filesystem::exists(old, ec)) return;
    }
    delete_registry_value(kRegCleanupOld);
}

std::filesystem::path cache_dir()         { return data_dir() / L"cache"; }
std::filesystem::path browser_cache_dir() { return cache_dir() / L"browser"; }
std::filesystem::path resources_dir()     { return data_dir() / L"resources"; }
std::filesystem::path tools_dir()         { return data_dir() / L"tools"; }

namespace {

void move_path_once(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return;
    if (std::filesystem::exists(dst, ec)) return;
    std::filesystem::create_directories(dst.parent_path(), ec);
    ec.clear();
    std::filesystem::rename(src, dst, ec);
    if (!ec) return;
    ec.clear();
    std::filesystem::copy(src, dst,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing |
        std::filesystem::copy_options::copy_symlinks, ec);
    if (!ec) std::filesystem::remove_all(src, ec);
}

}  // namespace

void migrate_data_layout() {
    const auto root = data_dir();
    std::error_code ec;

    std::filesystem::remove(root / L"browser-login.html", ec);

    move_path_once(root / L"browser-profile", browser_cache_dir() / L"profile");
    move_path_once(root / L"cs2_schema",      cache_dir() / L"cs2_schema");

    move_path_once(root / L"cs2_video_template.txt", resources_dir() / L"cs2_video_template.txt");
    move_path_once(root / L"cs2_730_template",       resources_dir() / L"cs2_730_template");

    move_path_once(root / L"gamesense", tools_dir() / L"gamesense");
    move_path_once(root / L"luminary",  tools_dir() / L"luminary");

    move_path_once(root / L"vaults.json", vaults_root() / L"registry.json");
}

void sweep_browser_cache() {
    std::error_code ec;
    std::filesystem::remove_all(browser_cache_dir(), ec);
}

void set_active_vault_id(const std::string& id) { g_active_vault_id = id; }
std::string active_vault_id() { return g_active_vault_id; }

std::filesystem::path vaults_root() { return data_dir() / L"vaults"; }

std::filesystem::path active_vault_dir() {

    return vaults_root() / std::filesystem::path(g_active_vault_id);
}

bool write_pending_vault(const std::string& id) {
    return write_registry_string(kRegPendingVault,
                                 std::wstring(id.begin(), id.end()));
}

std::string read_pending_vault() {
    const std::wstring w = read_registry_string(kRegPendingVault);
    std::string out;
    out.reserve(w.size());
    for (const wchar_t c : w) out.push_back(static_cast<char>(c & 0x7F));
    return out;
}

bool clear_pending_vault() { return delete_registry_value(kRegPendingVault); }

std::filesystem::path vault_path()    { return active_vault_dir() / L"vault.bin"; }
std::filesystem::path settings_path() { return data_dir() / L"settings.json"; }
std::filesystem::path log_dir()       { return data_dir() / L"logs"; }

}  // namespace sam::platform
