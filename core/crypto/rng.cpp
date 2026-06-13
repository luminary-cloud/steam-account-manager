#include "core/crypto/rng.hpp"

#include <array>
#include <stdexcept>

#include "core/strings.hpp"

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace sam::crypto {

namespace {

constexpr char kHexUpper[] = "0123456789ABCDEF";

void bcrypt_fill(std::uint8_t* buf, std::size_t n) {
    if (n == 0) return;
    const NTSTATUS status =
        BCryptGenRandom(nullptr, buf, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
}

}  // namespace

void fill_random(std::span<std::uint8_t> out) {
    bcrypt_fill(out.data(), out.size());
}

std::vector<std::uint8_t> random_bytes(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    bcrypt_fill(v.data(), v.size());
    return v;
}

std::string random_device_id() {
    // UUIDv4 layout: 8-4-4-4-12 hex digits, with the version (4) in nibble 13 and
    // the variant bits in nibble 17.
    std::array<std::uint8_t, 16> bytes{};
    bcrypt_fill(bytes.data(), bytes.size());
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);

    const std::string hex = core::to_hex_lower({bytes.data(), bytes.size()});
    std::string uuid;
    uuid.reserve(36);
    uuid += hex.substr(0, 8);
    uuid += '-';
    uuid += hex.substr(8, 4);
    uuid += '-';
    uuid += hex.substr(12, 4);
    uuid += '-';
    uuid += hex.substr(16, 4);
    uuid += '-';
    uuid += hex.substr(20, 12);

    return "android:" + uuid;
}

std::string random_session_id() {
    // SDA's GetRandomHexNumber(32): 32 uppercase hex from 16 bytes. Used as the
    // sessionid cookie and the CSRF value POSTed to /jwt/finalizelogin.
    auto bytes = random_bytes(16);
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[2 * i]     = kHexUpper[(bytes[i] >> 4) & 0x0F];
        out[2 * i + 1] = kHexUpper[bytes[i] & 0x0F];
    }
    return out;
}

}  // namespace sam::crypto
