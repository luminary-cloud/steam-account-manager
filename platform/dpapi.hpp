#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace sam::platform::dpapi {

struct DpapiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// CryptProtectData scoped to the current Windows user (no
// CRYPTPROTECT_LOCAL_MACHINE); only the same user can unprotect(). Throws DpapiError.
std::vector<std::uint8_t> protect(std::span<const std::uint8_t> plaintext);

// Throws DpapiError if the blob is corrupt or was protected under a different user.
std::vector<std::uint8_t> unprotect(std::span<const std::uint8_t> wrapped);

// Binds `plaintext` to `entropy` (DPAPI OptionalEntropy). Steam's local.vdf
// ConnectCache uses the account name as entropy, so the Steam client can decrypt.
std::vector<std::uint8_t> protect(std::span<const std::uint8_t> plaintext,
                                  std::span<const std::uint8_t> entropy);

}  // namespace sam::platform::dpapi
