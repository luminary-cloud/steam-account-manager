#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sam::steam_login {

struct RsaKey {
    std::string modulus_hex;
    std::string exponent_hex;
    std::uint64_t timestamp = 0;
};

// Fetches the per-account RSA key Steam uses to wrap passwords during the mobile
// login flow. Returns std::nullopt on HTTP failure or malformed response.
std::optional<RsaKey> fetch_rsa_key(const std::string& username);

// RSA-encrypts `password` with PKCS#1 v1.5 padding using the public key. Returns
// the base64-encoded ciphertext as Steam expects in `encrypted_password`.
std::string encrypt_password(const std::string& password, const RsaKey& key);

}  // namespace sam::steam_login
