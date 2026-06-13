#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sam::platform {

// Directory beside the .exe.
std::filesystem::path exe_dir();

// %LOCALAPPDATA%\steam-account-manager: used for config, vault, logs.
std::filesystem::path local_appdata_dir();

// Raw %LOCALAPPDATA% root, with no app subfolder. Used to locate other apps'
// data (e.g. Steam's local.vdf). Empty if it can't be resolved.
std::filesystem::path local_appdata_root();

// Returns the per-app data root, resolved once per process and cached.
// Resolution order: a `portable.flag` next to the .exe wins (-> exe_dir()/data);
// otherwise a valid custom location recorded in the registry (see
// set_custom_data_dir); otherwise local_appdata_dir(). This is pure - call
// ensure_data_dirs() once at startup to create the directory.
std::filesystem::path data_dir();

// The per-app data root ignoring any custom location (portable.flag or
// local_appdata_dir()). This is the location used when no custom folder is set.
std::filesystem::path default_data_dir();

// True when data_dir() resolved to a user-chosen folder rather than the default.
bool using_custom_data_dir();

// If a custom location was set but its target couldn't be used (e.g. a removed
// USB drive), returns that target and data_dir() falls back to default.
std::optional<std::filesystem::path> custom_data_dir_unavailable();

// Records/clears the custom data location in the registry (HKCU). Takes effect on
// the next launch. Kept in the registry rather than a file so a migration leaves
// nothing behind on disk.
bool set_custom_data_dir(const std::filesystem::path& dir, std::string* err);
bool clear_custom_data_dir(std::string* err);

// Copies the current data_dir() tree into new_dir, records the old folder for
// removal on the next launch, then records new_dir as the custom location
// (written last). Requires a restart to take effect. Returns false and fills err
// on validation/copy failure, leaving the current location intact.
bool relocate_data_dir(const std::filesystem::path& new_dir, std::string* err);

// If a previous relocation recorded an old folder, delete it now. Best-effort;
// call once early at startup (after ensure_data_dirs()).
void cleanup_relocated_old_dir();

// Convenience: data_dir() / "vault.bin".
std::filesystem::path vault_path();

// Convenience: data_dir() / "settings.json".
std::filesystem::path settings_path();

// Convenience: data_dir() / "logs".
std::filesystem::path log_dir();

}  // namespace sam::platform
