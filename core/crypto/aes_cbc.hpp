#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace sam::crypto {

struct CbcPaddingError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// AES-256-CBC with PKCS#7 padding. Used to read and write SDA-format encrypted
// maFiles. The vault itself uses GCM, not CBC.
std::vector<std::uint8_t> aes256_cbc_pkcs7_decrypt(std::span<const std::uint8_t> key,
                                                    std::span<const std::uint8_t> iv,
                                                    std::span<const std::uint8_t> ciphertext);

std::vector<std::uint8_t> aes256_cbc_pkcs7_encrypt(std::span<const std::uint8_t> key,
                                                    std::span<const std::uint8_t> iv,
                                                    std::span<const std::uint8_t> plaintext);

}  // namespace sam::crypto
