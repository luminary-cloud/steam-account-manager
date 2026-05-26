#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace sam::crypto {

// HMAC-SHA1 of `data` using `key`. Returns 20 bytes.
std::array<std::uint8_t, 20> hmac_sha1(std::span<const std::uint8_t> key,
                                       std::span<const std::uint8_t> data);

// HMAC-SHA256 of `data` using `key`. Returns 32 bytes.
std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::uint8_t> key,
                                          std::span<const std::uint8_t> data);

}  // namespace sam::crypto
