#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace sam::steam_local {

struct LocalAccount {
    std::uint64_t steam_id_64 = 0;
    std::string account_name;   // lowercased as Steam writes it
    std::string persona_name;
    std::int64_t timestamp = 0;
};

// Reads <SteamInstall>/config/loginusers.vdf. Empty on any failure.
std::vector<LocalAccount> read_loginusers();

// Case-insensitive AccountName lookup. 0 if no install, no file, or no match.
std::uint64_t lookup_steam_id(std::string_view login);

// Flags `steam_id_64` RememberPassword + AllowAutoLogin and clears them on every
// other entry, so Steam auto-logs into exactly this account. Other fields are
// preserved. Without these flags Steam ignores the
// HKCU\Software\Valve\Steam\AutoLoginUser hint and shows an empty login window.
// False if Steam isn't installed, the file is missing/unparseable, the steam_id
// isn't present, or the write fails.
bool set_remembered_account(std::uint64_t steam_id_64);

// Ensures loginusers.vdf has an entry for `steam_id_64`, flagged remembered /
// auto-login / most-recent, clearing those flags on every other entry. Creates
// the file, "users" block, or entry if missing. Used by the NFA token-login path.
bool ensure_loginusers_entry(std::uint64_t steam_id_64,
                             const std::string& account_name,
                             const std::string& persona_name);

// Ensures <SteamInstall>/config/config.vdf has the
// InstallConfigStore > Software > Valve > Steam > Accounts > <account_name> >
// SteamID entry that Steam reads to associate the AutoLoginUser name with its
// Steam ID. Without it the Steam client ignores the injected ConnectCache token
// and falls back to the login window. Used by the NFA token-login path. False if
// Steam isn't installed or the write fails.
bool ensure_config_vdf_account(std::uint64_t steam_id_64,
                               const std::string& account_name);

// Generic text-VDF tree, shared with the connect_cache writer. Round-trips
// unknown keys so a write preserves fields Valve may add.
struct VdfNode {
    std::string key;
    std::string value;             // populated iff !is_block
    std::vector<VdfNode> children;
    bool is_block = false;
};

VdfNode parse_vdf(std::string_view text);
void serialize_node(const VdfNode& node, std::string& out, int depth);
VdfNode* find_child(VdfNode& parent, std::string_view key);
void upsert_scalar(VdfNode& parent, std::string_view key, std::string_view value);

// Walks (creating as needed) a chain of nested block keys, returning the deepest.
VdfNode* ensure_block_path(VdfNode& root, std::initializer_list<const char*> path);

}  // namespace sam::steam_local
