#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/crypto/aes_cbc.hpp"   // re-uses CbcPaddingError

namespace sam::crypto {

// Rijndael-256 (256-bit block AND key) CBC + PKCS#7. NOT FIPS AES, which is
// fixed at a 128-bit block; this is the pre-standardisation variant exposed in
// .NET as RijndaelManaged{BlockSize=256,KeySize=256}, which the info.dat
// envelope uses. Params: Nb=8, Nk=8, Nr=14.
//
// `key` and `iv` are 32 bytes; `ciphertext` length must be a multiple of 32.
// Throws CbcPaddingError on bad PKCS#7 padding, which is also how a wrong
// password manifests.
std::vector<std::uint8_t> rijndael256_cbc_pkcs7_decrypt(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> iv,
    std::span<const std::uint8_t> ciphertext);

}  // namespace sam::crypto
