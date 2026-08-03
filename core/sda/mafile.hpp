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

// The maFile "Session" block carries SteamID64 and (often) tokens; captured
// here so callers don't have to re-authenticate to learn them.
struct MafileLoadResult {
    core::SteamGuardAccount guard;
    std::uint64_t session_steam_id = 0;
    std::string   session_access_token;   // JWT, may be empty
    std::string   session_refresh_token;  // JWT, may be empty
};

// Throws MafileEncrypted if the file is encrypted and `password` is empty;
// MafileWrongPassword if `password` is wrong.
MafileLoadResult load_mafile(const std::filesystem::path& path,
                             const crypto::SecureString& password);

}  // namespace sam::sda
