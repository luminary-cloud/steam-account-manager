#include "core/crypto/kdf.hpp"

#include <stdexcept>

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace sam::crypto {

std::vector<std::uint8_t> pbkdf2(KdfHash hash,
                                 std::span<const std::uint8_t> password,
                                 std::span<const std::uint8_t> salt,
                                 std::uint32_t iterations,
                                 std::size_t out_len) {
    if (iterations == 0) {
        throw std::invalid_argument("pbkdf2: iterations must be > 0");
    }
    if (out_len == 0) {
        return {};
    }

    const mbedtls_md_type_t md_type =
        (hash == KdfHash::Sha1) ? MBEDTLS_MD_SHA1 : MBEDTLS_MD_SHA256;

    const auto* info = mbedtls_md_info_from_type(md_type);
    if (!info) {
        throw std::runtime_error("pbkdf2: no md info");
    }

    std::vector<std::uint8_t> out(out_len);
    const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        md_type,
        password.data(), password.size(),
        salt.data(), salt.size(),
        iterations,
        static_cast<std::uint32_t>(out_len),
        out.data());
    if (rc != 0) {
        throw std::runtime_error("pbkdf2: mbedtls_pkcs5_pbkdf2_hmac_ext failed");
    }
    return out;
}

}  // namespace sam::crypto
