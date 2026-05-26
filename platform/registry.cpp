#include "platform/registry.hpp"

#include <filesystem>
#include <system_error>
#include <vector>

#include <windows.h>

namespace sam::platform::registry {

namespace {

constexpr wchar_t kSteamSubkey[] = L"Software\\Valve\\Steam";

}

std::optional<std::wstring> read_string_hkcu(const std::wstring& subkey,
                                              const std::wstring& value_name) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD type = 0;
    DWORD size = 0;
    LSTATUS rc = RegQueryValueExW(h, value_name.c_str(), nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(h);
        return std::nullopt;
    }

    std::wstring buf((size / sizeof(wchar_t)) + 1, L'\0');
    rc = RegQueryValueExW(h, value_name.c_str(), nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &size);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS) return std::nullopt;

    // Trim trailing NULs that the registry may include.
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

bool write_string_hkcu(const std::wstring& subkey,
                       const std::wstring& value_name,
                       const std::wstring& value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS rc = RegSetValueExW(h, value_name.c_str(), 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

std::optional<std::uint32_t> read_dword_hkcu(const std::wstring& subkey,
                                              const std::wstring& value_name) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS rc = RegQueryValueExW(h, value_name.c_str(), nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return std::nullopt;
    return value;
}

bool write_dword_hkcu(const std::wstring& subkey,
                      const std::wstring& value_name,
                      std::uint32_t value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    DWORD v = value;
    const LSTATUS rc = RegSetValueExW(h, value_name.c_str(), 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

std::optional<std::wstring> read_steam_exe_path() {
    auto v = read_string_hkcu(kSteamSubkey, L"SteamExe");
    if (v) return v;
    return read_string_hkcu(kSteamSubkey, L"SteamPath");
}

std::optional<std::filesystem::path> read_steam_install_dir() {
    std::error_code ec;
    if (auto sp = read_string_hkcu(kSteamSubkey, L"SteamPath")) {
        std::filesystem::path p(*sp);
        if (std::filesystem::is_directory(p, ec)) return p;
    }
    if (auto se = read_string_hkcu(kSteamSubkey, L"SteamExe")) {
        std::filesystem::path p(*se);
        // SteamExe is usually a file path to steam.exe; take its parent.
        if (std::filesystem::is_regular_file(p, ec)) {
            auto parent = p.parent_path();
            if (std::filesystem::is_directory(parent, ec)) return parent;
        } else if (std::filesystem::is_directory(p, ec)) {
            return p;
        }
    }
    return std::nullopt;
}

bool set_auto_login_user(const std::wstring& account_name) {
    return write_string_hkcu(kSteamSubkey, L"AutoLoginUser", account_name);
}

bool set_remember_password(bool remember) {
    return write_dword_hkcu(kSteamSubkey, L"RememberPassword", remember ? 1u : 0u);
}

}  // namespace sam::platform::registry
