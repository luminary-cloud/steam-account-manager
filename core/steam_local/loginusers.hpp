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

}  // namespace sam::steam_local
