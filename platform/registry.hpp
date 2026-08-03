#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// HKEY without dragging <windows.h> into every consumer; matches the SDK typedef.
struct HKEY__;
using HKEY = HKEY__*;

namespace sam::platform::registry {

// Hive-generic forms, for callers that touch more than HKCU (the tracer cleaner
// reaches into HKLM and HKCR). The _hkcu helpers below are the common case.
std::optional<std::wstring> read_string(HKEY root, const std::wstring& subkey,
                                         const std::wstring& value_name);
bool write_string(HKEY root, const std::wstring& subkey, const std::wstring& value_name,
                  const std::wstring& value);
// Both treat "already absent" as success.
bool delete_value(HKEY root, const std::wstring& subkey, const std::wstring& value_name);
bool delete_key_recursive(HKEY root, const std::wstring& subkey);

std::optional<std::wstring> read_string_hkcu(const std::wstring& subkey,
                                              const std::wstring& value_name);

bool write_string_hkcu(const std::wstring& subkey,
                       const std::wstring& value_name,
                       const std::wstring& value);

std::optional<std::uint32_t> read_dword_hkcu(const std::wstring& subkey,
                                              const std::wstring& value_name);

bool write_dword_hkcu(const std::wstring& subkey,
                      const std::wstring& value_name,
                      std::uint32_t value);

std::optional<std::wstring> read_steam_exe_path();
// Steam install dir (folder with steam.exe and config/). Prefers SteamPath, falls
// back to SteamExe's parent; verifies the path exists.
std::optional<std::filesystem::path> read_steam_install_dir();
bool set_auto_login_user(const std::wstring& account_name);
bool set_remember_password(bool remember);

// ActiveProcess\ActiveUser: account id (low 32 bits of SteamID) of the signed-in
// user, 0 when logged out. Client-version-independent way to detect login finished.
std::optional<std::uint32_t> read_active_user();

}  // namespace sam::platform::registry
