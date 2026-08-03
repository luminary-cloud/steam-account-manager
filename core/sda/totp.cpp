#include "core/sda/totp.hpp"

#include <array>
#include <cstdint>

#include "core/crypto/base64.hpp"
#include "core/crypto/hmac.hpp"
#include "core/time_aligner.hpp"

namespace sam::sda {

namespace {

constexpr char kAlphabet[] = "23456789BCDFGHJKMNPQRTVWXY";
constexpr std::size_t kAlphabetLen = sizeof(kAlphabet) - 1;
constexpr std::int64_t kWindowSeconds = 30;
constexpr std::size_t kCodeLen = 5;

}  // namespace

std::string generate_code(std::string_view shared_secret_b64, std::int64_t unix_time) {
    const auto key = crypto::base64_decode(shared_secret_b64);
    if (key.empty()) return {};

    const std::int64_t counter = unix_time / kWindowSeconds;
    std::array<std::uint8_t, 8> time_buf{};
    std::int64_t c = counter;
    for (int i = 7; i >= 0; --i) {
        time_buf[i] = static_cast<std::uint8_t>(c & 0xFF);
        c >>= 8;
    }

    const auto mac = crypto::hmac_sha1(
        std::span<const std::uint8_t>{key.data(), key.size()},
        std::span<const std::uint8_t>{time_buf.data(), time_buf.size()});

    const std::size_t offset = static_cast<std::size_t>(mac[19] & 0x0F);
    std::uint32_t value =
        ((static_cast<std::uint32_t>(mac[offset]) & 0x7F) << 24) |
        ((static_cast<std::uint32_t>(mac[offset + 1]) & 0xFF) << 16) |
        ((static_cast<std::uint32_t>(mac[offset + 2]) & 0xFF) << 8) |
        (static_cast<std::uint32_t>(mac[offset + 3]) & 0xFF);

    std::string code(kCodeLen, ' ');
    for (std::size_t i = 0; i < kCodeLen; ++i) {
        code[i] = kAlphabet[value % kAlphabetLen];
        value /= static_cast<std::uint32_t>(kAlphabetLen);
    }
    return code;
}

int seconds_remaining(std::int64_t unix_time) {
    const std::int64_t into_window = unix_time % kWindowSeconds;
    return static_cast<int>(kWindowSeconds - into_window);
}

std::string generate_code_now(std::string_view shared_secret_b64) {
    return generate_code(shared_secret_b64, time_aligner::aligned_now());
}

}  // namespace sam::sda
