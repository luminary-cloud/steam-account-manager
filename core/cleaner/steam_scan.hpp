#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sam::cleaner {

struct InstallInfo {
    std::filesystem::path install_path;    // C:/Program Files (x86)/Steam
    std::filesystem::path config_dir;      // <install>/config
    std::filesystem::path userdata_dir;    // <install>/userdata
    std::filesystem::path appcache_dir;    // <install>/appcache
    std::filesystem::path htmlcache_dir;   // %LOCALAPPDATA%/Steam/htmlcache
    std::filesystem::path local_vdf_path;  // %LOCALAPPDATA%/Steam/local.vdf
};

// Built on platform::registry::read_steam_install_dir(), so it agrees with the launcher about
// which Steam this machine has. nullopt when Steam isn't installed.
std::optional<InstallInfo> discover_install();

// Every library root from <install>/config/libraryfolders.vdf, including the primary install.
// Falls back to {install_path/steamapps} when the file is missing or malformed.
std::vector<std::filesystem::path> discover_libraries(const InstallInfo& install);

// A Steam account as it exists on this PC, not a vault account. The cleaner works off what
// Steam left on disk, which is a superset of what the vault knows about.
struct AccountInfo {
    std::wstring steamid64;        // "76561198..."
    std::uint32_t account_id = 0;  // SteamID3 lower 32 bits, the userdata folder name
    std::wstring account_name;     // login name from loginusers.vdf
    std::wstring persona_name;     // display name from loginusers.vdf
    bool most_recent = false;      // mostrecent flag in loginusers.vdf
    bool remember_password = false;
    std::filesystem::path userdata_path;  // <install>/userdata/<account_id>
};

// Enumerates <install>/userdata/* and merges metadata from loginusers.vdf.
std::vector<AccountInfo> enumerate_accounts(const InstallInfo& install);

// Resolves AutoLoginUser (an account name) to a SteamID64 via loginusers.vdf. Empty if absent.
std::wstring resolve_auto_login(const InstallInfo& install, std::wstring_view account_name);

std::uint32_t steamid64_to_account_id(std::wstring_view steamid64);
std::wstring account_id_to_steamid64(std::uint32_t account_id);

}  // namespace sam::cleaner
