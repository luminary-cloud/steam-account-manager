#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/crypto/secure_string.hpp"

namespace sam::sda {

// Field-encryption keys baked into released export-tool builds, tried in order
// by try_unwrap_field after any user-supplied key.
inline constexpr std::string_view kKnownInfoDatKeys[] = {
    "ImnsUDFnXghRF",
    "PRIVATE_KEY",  // open-source placeholder; used by rebuilds that left it unchanged
};

// One <Account> entry from an info.dat export; field names mirror the XML.
struct InfoDatAccount {
    std::string name;
    std::string password;          // may be empty
    std::string shared_secret;     // empty = no Steam Guard
    std::string steam_id_str;
    std::string description;
    std::string profile_url;
    std::string avatar_url;
    std::string alias;

    bool community_banned = false;
    bool vac_banned = false;
    int  vac_ban_count = 0;
    int  game_ban_count = 0;
    int  days_since_last_ban = 0;
    std::string economy_ban;
};

struct InfoDatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct InfoDatEncrypted : InfoDatError {
    using InfoDatError::InfoDatError;
};

struct InfoDatWrongPassword : InfoDatError {
    using InfoDatError::InfoDatError;
};

// `file_password` may be empty for unencrypted (plain XML) files. `field_key`
// wraps the inner <Password>/<SharedSecret> values; when empty (typical) the
// parser falls back to kKnownInfoDatKeys. Values that unwrap under no key are
// returned verbatim so the import still surfaces something.
//
// Throws InfoDatEncrypted if Base64-encrypted but `file_password` is empty,
// InfoDatWrongPassword on a bad file password, InfoDatError on other parse
// failure.
std::vector<InfoDatAccount> load_info_dat(const std::filesystem::path& path,
                                          const crypto::SecureString& file_password,
                                          std::string_view field_key = {});

// Test seam: parse already-decoded XML directly.
std::vector<InfoDatAccount> parse_info_dat_xml(std::string_view xml,
                                                std::string_view field_key = {});

// Unwraps a StringCipher envelope: Base64 of salt[32] | iv[32] |
// Rijndael-256-CBC-PKCS7 ciphertext, PBKDF2-SHA1 1000 iters. Tries `user_key`
// (if non-empty) then each kKnownInfoDatKeys constant, returning the cleartext
// from the first key that PKCS#7-unpads. Returns `value` unchanged if it is not
// envelope-shaped or no key works. Never throws.
std::string try_unwrap_field(std::string_view value, std::string_view user_key);

}  // namespace sam::sda
