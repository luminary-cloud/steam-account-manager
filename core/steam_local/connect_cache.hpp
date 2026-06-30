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

// Reads the raw ConnectCache value Steam stores for `account_name` (same key as
// write_connect_cache_token). Returned verbatim: still the lowercase-hex DPAPI
// blob, not the decrypted token. nullopt if local.vdf, the ConnectCache path, or
// the key is absent. Used to confirm Steam flushed the remember-me token and to
// re-inject it across a restart.
std::optional<std::string> read_connect_cache_value(const std::string& account_name);

// Reads and decrypts the ConnectCache login token Steam stored for `account_name`,
// returning the bare refresh-token JWT. Hex-decodes read_connect_cache_value, then
// DPAPI-unwraps it with entropy = the lowercased account name (how Steam wrapped it).
// nullopt if no value is stored, the hex is malformed, or DPAPI can't decrypt it
// (e.g. a different Windows user wrote it). The token is client-scoped, so the
// imported account behaves as NFA.
std::optional<crypto::SecureString> read_connect_cache_token(const std::string& account_name);

// Writes an already-wrapped ConnectCache value (as returned by
// read_connect_cache_value) back under the same key, verbatim and without
// re-wrapping. Steam must not be running.
bool write_connect_cache_raw(const std::string& account_name,
                             const std::string& raw_value);

}  // namespace sam::steam_local
