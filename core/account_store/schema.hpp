#pragma once

#include <array>
#include <cstdint>

namespace sam::core::store {

inline constexpr std::array<std::uint8_t, 4> kMagic = {'S', 'A', 'M', 'V'};
inline constexpr std::uint32_t kSchemaVersion = 3;
inline constexpr std::uint32_t kKdfPbkdf2Sha256 = 0;
inline constexpr std::uint32_t kDefaultKdfIterations = 600000;
inline constexpr std::size_t kSaltLen = 16;
inline constexpr std::size_t kNonceLen = 12;
inline constexpr std::size_t kTagLen = 16;
inline constexpr std::size_t kKeyLen = 32;

}  // namespace sam::core::store
