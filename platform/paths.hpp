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

// ---- Data-folder layout -----------------------------------------------------
// Grouping subfolders under data_dir(), so the root stays tidy.
std::filesystem::path cache_dir();          // data_dir()/cache   (regenerable)
std::filesystem::path browser_cache_dir();  // cache_dir()/browser (wiped on close)
std::filesystem::path resources_dir();      // data_dir()/resources (user templates)
std::filesystem::path tools_dir();          // data_dir()/tools   (downloaded loaders)

// Moves an older flat/legacy layout under data_dir() into the grouped subfolders
// above. Idempotent and best-effort; a locked folder is retried next launch.
void migrate_data_layout();

// Deletes the browser scratch (profile + login page). Best-effort: a still-open
// external browser locks its --user-data-dir, so a failed wipe is retried by the
// next-launch sweep. Recreated on demand at the next web login.
void sweep_browser_cache();

// ---- Multiple vaults --------------------------------------------------------
// The active vault id, set once per process before the vault is unlocked (a
// session binds to exactly one vault; switching restarts the app). Empty until
// set. active_vault_dir() = data_dir()/vaults/<id>.
void set_active_vault_id(const std::string& id);
std::string active_vault_id();
std::filesystem::path vaults_root();               // data_dir()/vaults
std::filesystem::path active_vault_dir();           // vaults_root()/<active id>

// Restart handoff: the vault a "switch vault" relaunch should open. Stored in
// the registry (HKCU), mirroring the custom-data-dir handoff. Read once and
// cleared at startup.
bool write_pending_vault(const std::string& id);
std::string read_pending_vault();
bool clear_pending_vault();

std::filesystem::path vault_path();

std::filesystem::path settings_path();

std::filesystem::path log_dir();

}  // namespace sam::platform
