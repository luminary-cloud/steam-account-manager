#include "core/crypto/hmac.hpp"

#include <stdexcept>

#include <mbedtls/md.h>

namespace sam::crypto {

namespace {

template <std::size_t N>
std::array<std::uint8_t, N> run(mbedtls_md_type_t type,
                                std::span<const std::uint8_t> key,
                                std::span<const std::uint8_t> data) {
    const auto* info = mbedtls_md_info_from_type(type);
    if (!info || mbedtls_md_get_size(info) != N) {
        throw std::runtime_error("hmac: md info mismatch");
    }

    std::array<std::uint8_t, N> out{};
    const int rc = mbedtls_md_hmac(info, key.data(), key.size(),
                                   data.data(), data.size(), out.data());
    if (rc != 0) {
        throw std::runtime_error("hmac: mbedtls_md_hmac failed");
    }
    return out;
}

}  // namespace

std::array<std::uint8_t, 20> hmac_sha1(std::span<const std::uint8_t> key,
                                       std::span<const std::uint8_t> data) {
    return run<20>(MBEDTLS_MD_SHA1, key, data);
}

std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::uint8_t> key,
                                          std::span<const std::uint8_t> data) {
    return run<32>(MBEDTLS_MD_SHA256, key, data);
}

}  // namespace sam::crypto
