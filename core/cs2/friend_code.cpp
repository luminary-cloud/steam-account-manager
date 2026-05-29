#include "core/cs2/friend_code.hpp"

#include <array>
#include <cstdint>

#include <mbedtls/md.h>

namespace sam::cs2 {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

std::uint32_t hash_steam_id(std::uint64_t steam_id) {
    const std::uint64_t account_id = steam_id & 0xFFFFFFFFull;
    const std::uint64_t strange = account_id | 0x4353474F00000000ull;

    std::array<unsigned char, 8> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] =
            static_cast<unsigned char>((strange >> (8 * i)) & 0xFF);
    }

    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (info == nullptr) return 0;

    std::array<unsigned char, 16> digest{};
    if (mbedtls_md(info, bytes.data(), bytes.size(), digest.data()) != 0) return 0;

    return static_cast<std::uint32_t>(digest[0])
         | (static_cast<std::uint32_t>(digest[1]) << 8)
         | (static_cast<std::uint32_t>(digest[2]) << 16)
         | (static_cast<std::uint32_t>(digest[3]) << 24);
}

std::uint64_t byteswap64(std::uint64_t v) {
    std::uint64_t out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | (v & 0xFF);
        v >>= 8;
    }
    return out;
}

}  // namespace

std::string friend_code(std::uint64_t steam_id_64) {
    if (steam_id_64 == 0) return {};

    const std::uint32_t h = hash_steam_id(steam_id_64);

    std::uint64_t id = steam_id_64;
    std::uint64_t r = 0;
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t id_nibble = id & 0xF;
        id >>= 4;
        const std::uint64_t hash_nibble = (static_cast<std::uint64_t>(h) >> i) & 1ull;
        const std::uint64_t a = (r << 4) | id_nibble;
        r = ((r >> 28) << 32) | a;
        r = ((r >> 31) << 32) | ((a << 1) | hash_nibble);
    }

    std::uint64_t v = byteswap64(r);
    std::string res;
    res.reserve(15);
    for (int i = 0; i < 13; ++i) {
        if (i == 4 || i == 9) res.push_back('-');
        res.push_back(kAlphabet[v & 0x1F]);
        v >>= 5;
    }
    if (res.size() >= 5 && res.compare(0, 4, "AAAA") == 0) {
        res = res.substr(5);
    }
    return res;
}

}  // namespace sam::cs2
