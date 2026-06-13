#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/crypto/secure_string.hpp"

namespace sam::steam_local {

// <LocalAppData>\Steam\local.vdf. nullopt if %LocalAppData% can't be resolved.
std::optional<std::filesystem::path> steam_local_vdf_path();

// Injects `refresh_token` into local.vdf so Steam signs into `account_name` on
// its next launch. Stored as Steam stores it: under
// MachineUserConfigStore/Software/Valve/Steam/ConnectCache, keyed by
// hex(crc32(lowercase account_name)) + "1", value = lowercase-hex of a DPAPI
// blob (CurrentUser, entropy = the account name). Steam must not be running (it
// rewrites local.vdf on exit).
bool write_connect_cache_token(const std::string& account_name,
                               const crypto::SecureString& refresh_token);

}  // namespace sam::steam_local
