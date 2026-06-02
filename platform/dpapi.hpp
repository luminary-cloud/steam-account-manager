#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace sam::platform::dpapi {

struct DpapiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Wraps `plaintext` via CryptProtectData, scoped to the current Windows user
// (no CRYPTPROTECT_LOCAL_MACHINE). The returned blob can be written to disk
// and decrypted later by unprotect() while running as the same user. Throws
// DpapiError on failure.
std::vector<std::uint8_t> protect(std::span<const std::uint8_t> plaintext);

// Unwraps a blob produced by protect(). Throws DpapiError if the blob is
// corrupt, was protected under a different Windows user, or the API rejects
// it for any other reason.
std::vector<std::uint8_t> unprotect(std::span<const std::uint8_t> wrapped);

// Wraps `plaintext` bound to `entropy` (DPAPI's OptionalEntropy). Steam's
// local.vdf ConnectCache uses the account name as entropy, so this lets us write
// a token the Steam client can decrypt.
std::vector<std::uint8_t> protect(std::span<const std::uint8_t> plaintext,
                                  std::span<const std::uint8_t> entropy);

}  // namespace sam::platform::dpapi
