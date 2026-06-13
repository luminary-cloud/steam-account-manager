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

// The chosen data location lives in the registry rather than a file, so a
// migration leaves nothing behind on disk in the old location.
constexpr const wchar_t* kRegSubkey     = L"Software\\steam-account-manager";
constexpr const wchar_t* kRegDataDir    = L"DataDir";     // active custom location
constexpr const wchar_t* kRegCleanupOld = L"CleanupOld";  // old dir to delete next launch

// Set during data_dir()'s one-time resolution when the registry named an
// unusable target (e.g. an unplugged drive). Read-only afterwards.
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

// Lowercased, separator-trimmed native string for case-insensitive comparison.
std::wstring norm_lower(const std::filesystem::path& p) {
    std::wstring s = p.lexically_normal().wstring();
    if (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool paths_equal(const std::filesystem::path& a, const std::filesystem::path& b) {
    return norm_lower(a) == norm_lower(b);
}

// True when `child` is `parent` or lives beneath it.
bool is_subpath(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const std::wstring c = norm_lower(child);
    const std::wstring p = norm_lower(parent);
    if (p.empty() || c.size() < p.size()) return false;
    if (c.compare(0, p.size(), p) != 0) return false;
    return c.size() == p.size() || c[p.size()] == L'\\' || c[p.size()] == L'/';
}

// Reads an absolute path from HKCU\kRegSubkey\value_name. Empty if the value is
// absent, blank, or not an absolute path.
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
    if (open == ERROR_FILE_NOT_FOUND) return true;  // nothing to clear
    if (open != ERROR_SUCCESS) return false;
    const LONG rc = RegDeleteValueW(key, value_name);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

}  // namespace

std::filesystem::path exe_dir() {
    return module_directory();
}

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

// Resolves the data root once. portable.flag wins; then a valid custom location
// from the registry; otherwise the default appdata dir. Records an unusable
// custom target so the UI can warn (e.g. a removed USB drive).
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
    data_dir();  // force the one-time resolution that sets the flag
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

    // Probe-write so we fail early on a read-only / full target.
    try {
        const auto probe = new_dir / L".sam_write_test";
        const std::uint8_t b = 0;
        atomic_write_file(probe, std::span<const std::uint8_t>(&b, 1));
        std::filesystem::remove(probe, ec);
    } catch (const std::exception& e) {
        return fail(std::string("Target folder isn't writable: ") + e.what());
    }

    // Copy the whole tree. The live log file is opened share-all by spdlog, so it
    // copies fine.
    ec.clear();
    std::filesystem::copy(current, new_dir,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing |
        std::filesystem::copy_options::copy_symlinks, ec);
    if (ec) return fail("Copy failed: " + ec.message());

    // Record the old folder so the next launch removes it (we can't delete it
    // now: this process still holds the log file open there). Stored in the
    // registry so nothing is left on disk in either location.
    write_registry_path(kRegCleanupOld, current);

    // Point at the new location last, so any failure above leaves us on the old.
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
        if (std::filesystem::exists(old, ec)) return;  // locked; retry next launch
    }
    delete_registry_value(kRegCleanupOld);
}

std::filesystem::path vault_path()    { return data_dir() / L"vault.bin"; }
std::filesystem::path settings_path() { return data_dir() / L"settings.json"; }
std::filesystem::path log_dir()       { return data_dir() / L"logs"; }

}  // namespace sam::platform
