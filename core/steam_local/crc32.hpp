#pragma once

#include <cstdint>
#include <string_view>

namespace sam::steam_local {

// Standard reflected CRC-32 (IEEE 802.3, poly 0xEDB88320), the zlib/Steam
// variant. Steam keys local.vdf ConnectCache entries by this CRC of the
// lowercase account name. Validated: crc32_ieee("123456789") == 0xCBF43926.
inline std::uint32_t crc32_ieee(std::string_view data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : data) {
        crc ^= ch;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace sam::steam_local
