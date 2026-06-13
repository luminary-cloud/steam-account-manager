#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace sam::platform::registry {

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
