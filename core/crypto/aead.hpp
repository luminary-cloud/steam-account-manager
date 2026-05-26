#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace sam::crypto {

struct AeadResult {
    std::vector<std::uint8_t> ciphertext;
    std::array<std::uint8_t, 16> tag{};
};

struct AeadDecryptError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// AES-256-GCM encryption. `key` is 32 bytes, `nonce` is 12 bytes.
AeadResult aes256_gcm_encrypt(std::span<const std::uint8_t> key,
                              std::span<const std::uint8_t> nonce,
                              std::span<const std::uint8_t> aad,
                              std::span<const std::uint8_t> plaintext);

// Decrypts and verifies the GCM tag. Throws AeadDecryptError on tag mismatch.
std::vector<std::uint8_t> aes256_gcm_decrypt(std::span<const std::uint8_t> key,
                                              std::span<const std::uint8_t> nonce,
                                              std::span<const std::uint8_t> aad,
                                              std::span<const std::uint8_t> ciphertext,
                                              std::span<const std::uint8_t> tag);

}  // namespace sam::crypto
