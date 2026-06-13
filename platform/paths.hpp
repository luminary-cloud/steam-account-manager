#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sam::platform {

std::filesystem::path exe_dir();

std::filesystem::path local_appdata_dir();

// Raw %LOCALAPPDATA% root, no app subfolder; used to locate other apps' data
// (e.g. Steam's local.vdf). Empty if it can't be resolved.
std::filesystem::path local_appdata_root();

// Per-app data root, resolved once and cached. portable.flag next to the .exe
// wins; else a valid custom registry location; else local_appdata_dir().
std::filesystem::path data_dir();

// Data root ignoring any custom location.
std::filesystem::path default_data_dir();

bool using_custom_data_dir();

// If a custom location was set but unusable (e.g. a removed USB drive), returns
// that target; data_dir() falls back to default.
std::optional<std::filesystem::path> custom_data_dir_unavailable();

// Records/clears the custom data location in the registry (HKCU); takes effect on
// next launch. In the registry, not a file, so a migration leaves nothing on disk.
bool set_custom_data_dir(const std::filesystem::path& dir, std::string* err);
bool clear_custom_data_dir(std::string* err);

// Copies the data_dir() tree into new_dir, records the old folder for removal on
// next launch, then records new_dir as the custom location (written last).
// Requires restart. Failure leaves the current location intact.
bool relocate_data_dir(const std::filesystem::path& new_dir, std::string* err);

// If a previous relocation recorded an old folder, delete it now. Best-effort.
void cleanup_relocated_old_dir();

std::filesystem::path vault_path();

std::filesystem::path settings_path();

std::filesystem::path log_dir();

}  // namespace sam::platform
