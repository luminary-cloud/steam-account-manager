#include "core/steam_login/rsa_password.hpp"

#include <cctype>
#include <cstring>
#include <vector>

#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/rsa.h>

#include <nlohmann/json.hpp>

#include "core/crypto/base64.hpp"
#include "core/http/client.hpp"
#include "core/http/url.hpp"
#include "core/log.hpp"

namespace sam::steam_login {

using json = nlohmann::json;

namespace {

std::vector<std::uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nybble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nybble(hex[i]);
        const int lo = nybble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

}  // namespace

std::optional<RsaKey> fetch_rsa_key(const std::string& username) {
    if (username.empty()) return std::nullopt;

    // GET request, JSON response shape. Steam also accepts a POST with a
    // protobuf body, but the response parser below expects JSON so we stick
    // with the GET form. (Mixing POST + ?account_name= query + empty body
    // returns HTTP 405.)
    http::Request req;
    req.method = http::Method::Get;
    req.url = "https://api.steampowered.com/IAuthenticationService/"
              "GetPasswordRSAPublicKey/v1/?account_name=" +
              http::url_encode(username);

    auto resp = http::request(req);
    if (resp.status != 200) {
        SAM_LOG_WARN("rsa: http {}", resp.status);
        return std::nullopt;
    }

    try {
        const auto j = json::parse(resp.body);
        if (!j.contains("response")) return std::nullopt;
        const auto& r = j["response"];

        RsaKey k;
        k.modulus_hex  = r.value("publickey_mod", "");
        k.exponent_hex = r.value("publickey_exp", "");
        if (r.contains("timestamp")) {
            if (r["timestamp"].is_string()) {
                k.timestamp = std::stoull(r["timestamp"].get<std::string>());
            } else {
                k.timestamp = r["timestamp"].get<std::uint64_t>();
            }
        }
        if (k.modulus_hex.empty() || k.exponent_hex.empty()) return std::nullopt;
        return k;
    } catch (...) {
        return std::nullopt;
    }
}

std::string encrypt_password(const std::string& password, const RsaKey& key) {
    if (password.empty() || key.modulus_hex.empty() || key.exponent_hex.empty()) {
        return {};
    }

    const auto mod_bytes = hex_to_bytes(key.modulus_hex);
    const auto exp_bytes = hex_to_bytes(key.exponent_hex);
    if (mod_bytes.empty() || exp_bytes.empty()) return {};

    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa);
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    auto cleanup = [&] {
        mbedtls_rsa_free(&rsa);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    };

    static const unsigned char pers[] = "sam-rsa-encrypt";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              pers, sizeof(pers) - 1) != 0) {
        cleanup();
        return {};
    }

    mbedtls_mpi N, E;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    auto mpi_cleanup = [&] {
        mbedtls_mpi_free(&N);
        mbedtls_mpi_free(&E);
    };

    if (mbedtls_mpi_read_binary(&N, mod_bytes.data(), mod_bytes.size()) != 0 ||
        mbedtls_mpi_read_binary(&E, exp_bytes.data(), exp_bytes.size()) != 0 ||
        mbedtls_rsa_import(&rsa, &N, nullptr, nullptr, nullptr, &E) != 0 ||
        mbedtls_rsa_complete(&rsa) != 0) {
        mpi_cleanup();
        cleanup();
        return {};
    }
    mpi_cleanup();

    const std::size_t mod_len = mbedtls_rsa_get_len(&rsa);
    std::vector<std::uint8_t> ct(mod_len);

    const int rc = mbedtls_rsa_pkcs1_encrypt(
        &rsa, mbedtls_ctr_drbg_random, &drbg,
        password.size(),
        reinterpret_cast<const unsigned char*>(password.data()),
        ct.data());

    cleanup();

    if (rc != 0) return {};
    return crypto::base64_encode(std::span<const std::uint8_t>{ct.data(), ct.size()});
}

}  // namespace sam::steam_login
