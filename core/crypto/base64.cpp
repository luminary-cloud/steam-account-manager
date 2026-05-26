#include "core/crypto/base64.hpp"

#include <array>
#include <cstdint>

namespace sam::crypto {

namespace {

constexpr char kStdAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

std::array<std::int8_t, 256> build_decode_table() {
    std::array<std::int8_t, 256> t{};
    for (auto& v : t) v = -1;
    for (std::int8_t i = 0; i < 64; ++i) {
        t[static_cast<std::uint8_t>(kStdAlphabet[i])] = i;
    }
    // Accept URL-safe variants in the same decoder.
    t[static_cast<std::uint8_t>('-')] = 62;
    t[static_cast<std::uint8_t>('_')] = 63;
    return t;
}

const std::array<std::int8_t, 256>& decode_table() {
    static const auto table = build_decode_table();
    return table;
}

std::string encode_with_alphabet(std::span<const std::uint8_t> bytes,
                                 const char* alpha,
                                 bool pad) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                          (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                          static_cast<std::uint32_t>(bytes[i + 2]);
        out += alpha[(n >> 18) & 0x3F];
        out += alpha[(n >> 12) & 0x3F];
        out += alpha[(n >> 6) & 0x3F];
        out += alpha[n & 0x3F];
        i += 3;
    }

    const std::size_t rem = bytes.size() - i;
    if (rem == 1) {
        std::uint32_t n = static_cast<std::uint32_t>(bytes[i]) << 16;
        out += alpha[(n >> 18) & 0x3F];
        out += alpha[(n >> 12) & 0x3F];
        if (pad) out += "==";
    } else if (rem == 2) {
        std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                          (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out += alpha[(n >> 18) & 0x3F];
        out += alpha[(n >> 12) & 0x3F];
        out += alpha[(n >> 6) & 0x3F];
        if (pad) out += '=';
    }

    return out;
}

}  // namespace

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    return encode_with_alphabet(bytes, kStdAlphabet, true);
}

std::string base64_encode(std::string_view bytes) {
    return base64_encode(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

std::vector<std::uint8_t> base64_decode(std::string_view encoded) {
    const auto& t = decode_table();

    std::vector<std::uint8_t> out;
    out.reserve((encoded.size() / 4) * 3);

    std::uint32_t buf = 0;
    int bits = 0;

    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        const std::int8_t v = t[static_cast<std::uint8_t>(c)];
        if (v < 0) {
            return {};  // Malformed
        }
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFF));
        }
    }

    return out;
}

std::string base64_url_query_encode(std::string_view base64) {
    std::string out;
    out.reserve(base64.size() + 16);
    for (char c : base64) {
        switch (c) {
            case '+': out += "%2B"; break;
            case '/': out += "%2F"; break;
            case '=': out += "%3D"; break;
            default:  out += c;
        }
    }
    return out;
}

}  // namespace sam::crypto
