#pragma once

#include <filesystem>
#include <string>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::sda {

// Writes one .maFile in the SDA encrypted layout (AES-256-CBC + PBKDF2-SHA1)
// to `path`. The shape matches what SteamDesktopAuthenticator produces, so the
// file round-trips through `load_mafile`. Returns true on success; on failure
// fills `err` if non-null and returns false. Passphrase must be non-empty.
bool encrypt_and_write_mafile(const std::filesystem::path& path,
                              const core::Account& account,
                              const crypto::SecureString& passphrase,
                              std::string* err);

// Builds an otpauth:// URI from a stored shared_secret (base64). Fallback for
// accounts whose `uri` field is empty, which happens with older maFile
// imports. Returns an empty string if shared_secret is empty or malformed.
// digits=5 matches Steam's TOTP shape.
std::string synthesize_otpauth_uri(const std::string& login,
                                   const std::string& shared_secret_b64);

}  // namespace sam::sda
