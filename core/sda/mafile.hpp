#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::sda {

struct MafileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct MafileEncrypted : MafileError {
    using MafileError::MafileError;
};

struct MafileWrongPassword : MafileError {
    using MafileError::MafileError;
};

// Parsed maFile contents. Beyond the bare TOTP fields, real maFiles carry a
// "Session" block with the user's SteamID64 and (often) Steam access/refresh
// tokens: capture those here so callers don't have to re-authenticate to learn
// them.
struct MafileLoadResult {
    core::SteamGuardAccount guard;
    std::uint64_t session_steam_id = 0;
    std::string   session_access_token;   // JWT, may be empty
    std::string   session_refresh_token;  // JWT, may be empty
};

// Reads a maFile from disk. If the file is encrypted and `password` is empty,
// throws MafileEncrypted so the caller can prompt the user. If `password` is
// wrong, throws MafileWrongPassword.
MafileLoadResult load_mafile(const std::filesystem::path& path,
                             const crypto::SecureString& password);

// Parses an UNENCRYPTED maFile JSON blob. Throws MafileEncrypted if the blob
// is in the encrypted layout.
MafileLoadResult parse_mafile_json(std::string_view json_text);

}  // namespace sam::sda
