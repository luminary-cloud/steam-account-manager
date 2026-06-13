#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace sam::crypto {

std::array<std::uint8_t, 20> hmac_sha1(std::span<const std::uint8_t> key,
                                       std::span<const std::uint8_t> data);

std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::uint8_t> key,
                                          std::span<const std::uint8_t> data);

}  // namespace sam::crypto
