#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/crypto/aes_cbc.hpp"   // re-uses CbcPaddingError

namespace sam::crypto {

// Rijndael with 256-bit block and 256-bit key in CBC mode with PKCS#7 padding.
// This is NOT the FIPS AES cipher (AES is fixed at 128-bit block); it is the
// pre-standardisation Rijndael variant still exposed in .NET as
// `RijndaelManaged { BlockSize = 256, KeySize = 256 }`. The info.dat envelope
// uses this exact configuration, so we implement it here just for the legacy
// import path.
//
// Parameters: Nb=8, Nk=8, Nr=14. ShiftRows offsets for Nb=8 are {0,1,3,4}.
//
// Both `key` and `iv` are 32 bytes (256 bits). `ciphertext` length must be a
// multiple of 32. Throws `CbcPaddingError` on malformed PKCS#7 padding (which
// is also how a wrong password manifests).
std::vector<std::uint8_t> rijndael256_cbc_pkcs7_decrypt(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> iv,
    std::span<const std::uint8_t> ciphertext);

}  // namespace sam::crypto
