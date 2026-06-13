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

std::optional<RsaKey> fetch_rsa_key(const std::string& username);

// PKCS#1 v1.5 padding; base64 ciphertext as Steam expects in `encrypted_password`.
std::string encrypt_password(const std::string& password, const RsaKey& key);

}  // namespace sam::steam_login
