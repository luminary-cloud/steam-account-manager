#include "core/crypto/aes_cbc.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

#include <mbedtls/aes.h>

namespace sam::crypto {

namespace {

constexpr std::size_t kBlockSize = 16;

struct AesCtx {
    mbedtls_aes_context ctx{};
    AesCtx()  { mbedtls_aes_init(&ctx); }
    ~AesCtx() { mbedtls_aes_free(&ctx); }
    AesCtx(const AesCtx&) = delete;
    AesCtx& operator=(const AesCtx&) = delete;
};

void check_args(std::span<const std::uint8_t> key, std::span<const std::uint8_t> iv) {
    if (key.size() != 32) {
        throw std::invalid_argument("aes256_cbc: key must be 32 bytes");
    }
    if (iv.size() != 16) {
        throw std::invalid_argument("aes256_cbc: iv must be 16 bytes");
    }
}

}  // namespace

std::vector<std::uint8_t> aes256_cbc_pkcs7_decrypt(std::span<const std::uint8_t> key,
                                                    std::span<const std::uint8_t> iv,
                                                    std::span<const std::uint8_t> ciphertext) {
    check_args(key, iv);
    if (ciphertext.size() == 0 || ciphertext.size() % kBlockSize != 0) {
        throw CbcPaddingError("aes256_cbc: ciphertext length must be a multiple of 16");
    }

    AesCtx a;
    if (mbedtls_aes_setkey_dec(&a.ctx, key.data(), 256) != 0) {
        throw std::runtime_error("aes256_cbc: setkey_dec failed");
    }

    std::vector<std::uint8_t> out(ciphertext.size());
    std::array<std::uint8_t, 16> iv_copy{};
    std::memcpy(iv_copy.data(), iv.data(), 16);

    const int rc = mbedtls_aes_crypt_cbc(&a.ctx, MBEDTLS_AES_DECRYPT,
                                          ciphertext.size(),
                                          iv_copy.data(),
                                          ciphertext.data(),
                                          out.data());
    if (rc != 0) {
        throw std::runtime_error("aes256_cbc: decrypt failed");
    }

    const std::uint8_t pad = out.back();
    if (pad == 0 || pad > kBlockSize || pad > out.size()) {
        throw CbcPaddingError("aes256_cbc: invalid PKCS#7 padding");
    }
    for (std::size_t i = out.size() - pad; i < out.size(); ++i) {
        if (out[i] != pad) {
            throw CbcPaddingError("aes256_cbc: corrupt PKCS#7 padding");
        }
    }
    out.resize(out.size() - pad);
    return out;
}

std::vector<std::uint8_t> aes256_cbc_pkcs7_encrypt(std::span<const std::uint8_t> key,
                                                    std::span<const std::uint8_t> iv,
                                                    std::span<const std::uint8_t> plaintext) {
    check_args(key, iv);

    const std::size_t pad = kBlockSize - (plaintext.size() % kBlockSize);
    std::vector<std::uint8_t> padded(plaintext.size() + pad);
    if (!plaintext.empty()) {
        std::memcpy(padded.data(), plaintext.data(), plaintext.size());
    }
    std::memset(padded.data() + plaintext.size(), static_cast<std::uint8_t>(pad), pad);

    AesCtx a;
    if (mbedtls_aes_setkey_enc(&a.ctx, key.data(), 256) != 0) {
        throw std::runtime_error("aes256_cbc: setkey_enc failed");
    }

    std::vector<std::uint8_t> out(padded.size());
    std::array<std::uint8_t, 16> iv_copy{};
    std::memcpy(iv_copy.data(), iv.data(), 16);

    const int rc = mbedtls_aes_crypt_cbc(&a.ctx, MBEDTLS_AES_ENCRYPT,
                                          padded.size(),
                                          iv_copy.data(),
                                          padded.data(),
                                          out.data());
    if (rc != 0) {
        throw std::runtime_error("aes256_cbc: encrypt failed");
    }
    return out;
}

}  // namespace sam::crypto
