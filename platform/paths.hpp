#pragma once

#include <filesystem>

namespace sam::platform {

// Directory beside the .exe.
std::filesystem::path exe_dir();

// %LOCALAPPDATA%\steam-account-manager: used for config, vault, logs.
std::filesystem::path local_appdata_dir();

// Raw %LOCALAPPDATA% root, with no app subfolder. Used to locate other apps'
// data (e.g. Steam's local.vdf). Empty if it can't be resolved.
std::filesystem::path local_appdata_root();

// Returns the per-app data root. If a `portable.flag` file exists next to the
// .exe, this is exe_dir(); otherwise it's local_appdata_dir().
std::filesystem::path data_dir();

// Convenience: data_dir() / "vault.bin".
std::filesystem::path vault_path();

// Convenience: data_dir() / "settings.json".
std::filesystem::path settings_path();

// Convenience: data_dir() / "logs".
std::filesystem::path log_dir();

}  // namespace sam::platform
