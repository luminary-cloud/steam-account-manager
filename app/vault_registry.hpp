#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sam::app {

struct AppState;

// One vault's identity + presentation. The account data itself lives in the
// vault's own encrypted vault.bin; this is just the metadata shown in the picker
// and the settings list.
struct VaultInfo {
    std::string id;          // stable, folder name under data_dir()/vaults/
    std::string name;
    std::string icon_file;   // bare filename in the vault dir (e.g. "icon.png"), or empty
    std::uint32_t color_rgba = 0x4F8EF7FFu;  // muted blue, matches Tag/Group default
    int order = 0;
    std::int64_t last_opened_unix = 0;
};

struct VaultRegistry {
    int version = 1;
    std::string auto_open_id;           // "" = show the picker when >1 vault
    std::vector<VaultInfo> vaults;

    VaultInfo* find(const std::string& id);
    const VaultInfo* find(const std::string& id) const;
};

// How startup should proceed once the registry is resolved.
enum class VaultResolution {
    OpenDirect,   // exactly one vault (or a switch handoff): open it, no picker
    AutoUnlock,   // multiple vaults, an auto-open vault with a cache: open directly
    ShowPicker,   // multiple vaults, let the user choose
    CreateFirst,  // no vaults yet: run the create flow for the first one
};

std::string new_vault_id();
std::filesystem::path vaults_json_path();
std::filesystem::path vault_dir_for(const std::string& id);

VaultRegistry load_registry();
void save_registry(const VaultRegistry& reg);

// Mutations, each persisting the registry. add_vault_entry takes an explicit id so
// the caller can create the vault file first, then register it.
void add_vault_entry(VaultRegistry& reg, std::string id, std::string name,
                     std::uint32_t color);
void rename_vault(VaultRegistry& reg, const std::string& id, std::string name);
// Copies `src` into the vault folder as icon.png and records it. false + *err on failure.
bool set_vault_icon(VaultRegistry& reg, const std::string& id,
                    const std::filesystem::path& src, std::string* err);
void clear_vault_icon(VaultRegistry& reg, const std::string& id);
// Deletes the vault's folder and drops its entry. Refuses to remove the last
// vault. false + *err if disallowed or on error.
bool remove_vault(VaultRegistry& reg, const std::string& id, std::string* err);
void set_auto_open(VaultRegistry& reg, const std::string& id);  // "" clears

// One-time startup: migrate a legacy single vault into a vault folder, load the
// registry into state.vault_registry, choose the active vault and call
// platform::set_active_vault_id() (except in the ShowPicker case, where the
// picker commits the choice). `gui` toggles the pending-switch handoff and the
// picker; a headless run treats ShowPicker/CreateFirst as "nothing to open".
VaultResolution init_vaults(AppState& state, bool gui);

}  // namespace sam::app
