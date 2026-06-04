#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace sam::platform::registry {

// HKCU\Software\Valve\Steam helpers.

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

// Convenience for Steam's specific keys.
std::optional<std::wstring> read_steam_exe_path();
// Returns the Steam install directory (i.e. the folder containing steam.exe and config/).
// Prefers SteamPath; falls back to SteamExe's parent. Verifies the resulting path exists.
std::optional<std::filesystem::path> read_steam_install_dir();
bool set_auto_login_user(const std::wstring& account_name);
bool set_remember_password(bool remember);

// HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser: the account id (low 32
// bits of the SteamID) of the currently signed-in user, or 0 when nobody is
// logged in. The reliable, client-version-independent way to detect that a
// Steam login finished.
std::optional<std::uint32_t> read_active_user();

}  // namespace sam::platform::registry
