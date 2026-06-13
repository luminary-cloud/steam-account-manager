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
    "PRIVATE_KEY",  // open-source placeholder; rebuilds that forgot to change it use this
};

// One <Account> entry from an info.dat export. Field names mirror the XML
// schema. Any fields not present in the XML default to the values set here.
struct InfoDatAccount {
    std::string name;              // -> Account::login
    std::string password;          // -> Account::password (may be empty)
    std::string shared_secret;     // -> Account::sda->shared_secret (empty = no Steam Guard)
    std::string steam_id_str;      // -> Account::steam_id_64 (parsed)
    std::string description;       // -> Account::notes
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

// Thrown when the file is encrypted but no password was supplied.
struct InfoDatEncrypted : InfoDatError {
    using InfoDatError::InfoDatError;
};

// Thrown when the supplied password did not decrypt the file (PKCS#7 padding
// failed).
struct InfoDatWrongPassword : InfoDatError {
    using InfoDatError::InfoDatError;
};

// Loads an info.dat file. `file_password` may be empty if the file is
// unencrypted (plain XML). `field_key` is the per-build constant used to wrap
// the inner <Password> / <SharedSecret> XML element values. When it is empty
// (the typical case), the parser falls back to a built-in try-list of known
// eKeys (see kKnownInfoDatKeys). Values that do not unwrap under any of the
// attempted keys are returned verbatim so the import still surfaces something.
//
// Throws:
//  - InfoDatEncrypted  if the file looks Base64-encrypted but `file_password` is empty
//  - InfoDatWrongPassword on bad file-level password
//  - InfoDatError on any other parse failure (missing root element, etc.)
std::vector<InfoDatAccount> load_info_dat(const std::filesystem::path& path,
                                          const crypto::SecureString& file_password,
                                          std::string_view field_key = {});

// Test seam: parse already-decoded XML directly.
std::vector<InfoDatAccount> parse_info_dat_xml(std::string_view xml,
                                                std::string_view field_key = {});

// Tries to unwrap `value` as a StringCipher envelope (Base64 of
// salt[32] | iv[32] | Rijndael-256-CBC-PKCS7 ciphertext, PBKDF2-SHA1 1000
// iters keyed by `user_key` or each known eKey).
//
// Try order:
//   1. `user_key`, if non-empty.
//   2. Every constant in kKnownInfoDatKeys.
// Returns the cleartext on the first key that successfully PKCS#7-unpads.
// Returns `value` unchanged if `value` is not envelope-shaped or no key
// works. Never throws.
std::string try_unwrap_field(std::string_view value, std::string_view user_key);

}  // namespace sam::sda
