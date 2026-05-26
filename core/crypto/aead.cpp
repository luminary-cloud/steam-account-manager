#include "core/crypto/aead.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

#include <mbedtls/gcm.h>

namespace sam::crypto {

namespace {

struct GcmCtx {
    mbedtls_gcm_context ctx{};

    GcmCtx() {
        mbedtls_gcm_init(&ctx);
    }

    ~GcmCtx() {
        mbedtls_gcm_free(&ctx);
    }

    GcmCtx(const GcmCtx&) = delete;
    GcmCtx& operator=(const GcmCtx&) = delete;
};

void check_key_nonce(std::span<const std::uint8_t> key,
                     std::span<const std::uint8_t> nonce) {
    if (key.size() != 32) {
        throw std::invalid_argument("aes256_gcm: key must be 32 bytes");
    }
    if (nonce.size() != 12) {
        throw std::invalid_argument("aes256_gcm: nonce must be 12 bytes");
    }
}

}  // namespace

AeadResult aes256_gcm_encrypt(std::span<const std::uint8_t> key,
                              std::span<const std::uint8_t> nonce,
                              std::span<const std::uint8_t> aad,
                              std::span<const std::uint8_t> plaintext) {
    check_key_nonce(key, nonce);

    GcmCtx g;
    if (mbedtls_gcm_setkey(&g.ctx, MBEDTLS_CIPHER_ID_AES,
                           key.data(), 256) != 0) {
        throw std::runtime_error("aes256_gcm: setkey failed");
    }

    AeadResult res;
    res.ciphertext.resize(plaintext.size());

    const int rc = mbedtls_gcm_crypt_and_tag(
        &g.ctx, MBEDTLS_GCM_ENCRYPT, plaintext.size(),
        nonce.data(), nonce.size(),
        aad.data(), aad.size(),
        plaintext.data(), res.ciphertext.data(),
        res.tag.size(), res.tag.data());
    if (rc != 0) {
        throw std::runtime_error("aes256_gcm: encrypt failed");
    }
    return res;
}

std::vector<std::uint8_t> aes256_gcm_decrypt(std::span<const std::uint8_t> key,
                                              std::span<const std::uint8_t> nonce,
                                              std::span<const std::uint8_t> aad,
                                              std::span<const std::uint8_t> ciphertext,
                                              std::span<const std::uint8_t> tag) {
    check_key_nonce(key, nonce);
    if (tag.size() != 16) {
        throw std::invalid_argument("aes256_gcm: tag must be 16 bytes");
    }

    GcmCtx g;
    if (mbedtls_gcm_setkey(&g.ctx, MBEDTLS_CIPHER_ID_AES,
                           key.data(), 256) != 0) {
        throw std::runtime_error("aes256_gcm: setkey failed");
    }

    std::vector<std::uint8_t> plaintext(ciphertext.size());
    const int rc = mbedtls_gcm_auth_decrypt(
        &g.ctx, ciphertext.size(),
        nonce.data(), nonce.size(),
        aad.data(), aad.size(),
        tag.data(), tag.size(),
        ciphertext.data(), plaintext.data());

    if (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) {
        throw AeadDecryptError("aes256_gcm: tag mismatch");
    }
    if (rc != 0) {
        throw std::runtime_error("aes256_gcm: decrypt failed");
    }
    return plaintext;
}

}  // namespace sam::crypto
