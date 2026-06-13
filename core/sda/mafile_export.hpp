#pragma once

#include <filesystem>
#include <string>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::sda {

// Writes one .maFile in the SDA encrypted layout (AES-256-CBC + PBKDF2-SHA1),
// matching SteamDesktopAuthenticator so it round-trips through load_mafile.
// Passphrase must be non-empty. On failure fills `err` if non-null.
bool encrypt_and_write_mafile(const std::filesystem::path& path,
                              const core::Account& account,
                              const crypto::SecureString& passphrase,
                              std::string* err);

// Fallback for accounts whose `uri` field is empty (older maFile imports).
// Returns empty if shared_secret is empty or malformed. digits=5 = Steam shape.
std::string synthesize_otpauth_uri(const std::string& login,
                                   const std::string& shared_secret_b64);

}  // namespace sam::sda
