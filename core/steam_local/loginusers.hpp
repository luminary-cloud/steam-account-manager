#pragma once

#include <cstdint>
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

// Reads <SteamInstall>/config/loginusers.vdf. Returns empty on any failure
// (missing file, parse error, no install detected).
std::vector<LocalAccount> read_loginusers();

// Case-insensitive AccountName lookup against loginusers.vdf.
// Returns 0 if no install is present, the file is missing, or no match.
std::uint64_t lookup_steam_id(std::string_view login);

// Sets RememberPassword + AllowAutoLogin on the entry for `steam_id_64` and
// clears those flags on every other entry, so Steam auto-logs into exactly
// the requested account. All other fields in each entry are preserved.
//
// Mirrors what the Steam client itself writes when "Remember my password"
// is checked at sign-in. Without these flags set in the VDF, Steam ignores
// the HKCU\Software\Valve\Steam\AutoLoginUser hint and shows an empty login
// window.
//
// Returns true on success. Returns false if Steam isn't installed, the file
// is missing, the file can't be parsed, the steam_id isn't present in the
// file, or the write fails.
bool set_remembered_account(std::uint64_t steam_id_64);

// Ensures loginusers.vdf has an entry for `steam_id_64` with `account_name`
// (and `persona_name`, if non-empty), flags it RememberPassword / AllowAutoLogin
// / MostRecent, and clears those flags on every other entry. Creates the file,
// the "users" block, or the entry if missing. Used by the NFA token-login path
// so Steam treats the injected account as remembered. Returns true on success.
bool ensure_loginusers_entry(std::uint64_t steam_id_64,
                             const std::string& account_name,
                             const std::string& persona_name);

// Generic text-VDF tree, shared with the connect_cache / config writers. Only
// the subset we need: parse, look up / upsert scalars, and re-serialize.
// Round-trips unknown keys so a write preserves fields Valve may add.
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

}  // namespace sam::steam_local
