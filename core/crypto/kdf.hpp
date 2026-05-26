#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sam::crypto {

enum class KdfHash {
    Sha1,
    Sha256,
};

// PBKDF2 with the given hash. Returns `out_len` bytes.
std::vector<std::uint8_t> pbkdf2(KdfHash hash,
                                 std::span<const std::uint8_t> password,
                                 std::span<const std::uint8_t> salt,
                                 std::uint32_t iterations,
                                 std::size_t out_len);

}  // namespace sam::crypto
