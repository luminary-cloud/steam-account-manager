#include "core/sda/mafile_export.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

#include "core/crypto/aes_cbc.hpp"
#include "core/crypto/base64.hpp"
#include "core/crypto/kdf.hpp"
#include "core/crypto/rng.hpp"

namespace sam::sda {

using json = nlohmann::json;

namespace {

constexpr std::uint32_t kSdaPbkdfIterations = 50000;
constexpr std::size_t kSdaKeyLen  = 32;
constexpr std::size_t kSdaIvLen   = 16;
constexpr std::size_t kSdaSaltLen = 8;

std::vector<std::uint8_t> derive_sda_key(const crypto::SecureString& password,
                                          std::span<const std::uint8_t> salt,
                                          std::uint32_t iterations) {
    std::span<const std::uint8_t> pw(
        reinterpret_cast<const std::uint8_t*>(password.data()), password.size());
    return crypto::pbkdf2(crypto::KdfHash::Sha1, pw, salt, iterations, kSdaKeyLen);
}

json build_inner_json(const core::Account& a) {
    const auto& s = *a.sda;
    json inner;
    inner["shared_secret"]   = s.shared_secret;
    inner["serial_number"]   = s.serial_number;
    inner["revocation_code"] = s.revocation_code;
    inner["uri"]             = s.uri;
    inner["server_time"]     = s.server_time;
    inner["account_name"]    = s.account_name.empty() ? a.login : s.account_name;
    inner["token_gid"]       = s.token_gid;
    inner["identity_secret"] = s.identity_secret;
    inner["secret_1"]        = s.secret_1;
    inner["status"]          = s.status;
    inner["device_id"]       = s.device_id;
    inner["fully_enrolled"]  = s.fully_enrolled;

    json sess = json::object();
    if (a.steam_id_64 != 0) sess["SteamID"] = a.steam_id_64;
    if (!a.access_token.empty()) {
        sess["AccessToken"] = std::string(a.access_token.begin(), a.access_token.end());
    }
    if (!a.refresh_token.empty()) {
        sess["RefreshToken"] = std::string(a.refresh_token.begin(), a.refresh_token.end());
    }
    inner["Session"] = sess;
    return inner;
}

}  // namespace

bool encrypt_and_write_mafile(const std::filesystem::path& path,
                              const core::Account& account,
                              const crypto::SecureString& passphrase,
                              std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };

    if (!account.sda.has_value())        return fail("account has no Steam Guard data");
    if (passphrase.empty())              return fail("passphrase must not be empty");

    const auto inner = build_inner_json(account);
    const std::string inner_text = inner.dump();
    const std::span<const std::uint8_t> plain_span(
        reinterpret_cast<const std::uint8_t*>(inner_text.data()), inner_text.size());

    const auto salt = crypto::random_bytes(kSdaSaltLen);
    const auto iv   = crypto::random_bytes(kSdaIvLen);
    auto key = derive_sda_key(passphrase,
                              std::span<const std::uint8_t>{salt.data(), salt.size()},
                              kSdaPbkdfIterations);

    std::vector<std::uint8_t> ct;
    try {
        ct = crypto::aes256_cbc_pkcs7_encrypt(
            std::span<const std::uint8_t>{key.data(), key.size()},
            std::span<const std::uint8_t>{iv.data(),  iv.size()},
            plain_span);
    } catch (const std::exception& ex) {
        crypto::zero_buffer(key.data(), key.size());
        return fail(ex.what());
    }
    crypto::zero_buffer(key.data(), key.size());

    json out;
    out["encrypted"]               = crypto::base64_encode(ct);
    out["encryption_iv"]           = crypto::base64_encode(iv);
    out["encryption_salt"]         = crypto::base64_encode(salt);
    out["Encryption_Iterations"]   = static_cast<int>(kSdaPbkdfIterations);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail("could not open output file for write");
    f << out.dump();
    if (!f) return fail("write to output file failed");
    return true;
}

namespace {

std::string base32_encode(std::span<const std::uint8_t> bytes) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out;
    out.reserve(((bytes.size() + 4) / 5) * 8);
    std::uint64_t buf = 0;
    int bits = 0;
    for (auto b : bytes) {
        buf = (buf << 8) | b;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kAlphabet[(buf >> bits) & 0x1F]);
        }
    }
    if (bits > 0) {
        out.push_back(kAlphabet[(buf << (5 - bits)) & 0x1F]);
    }
    while (out.size() % 8 != 0) out.push_back('=');
    return out;
}

std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool unreserved = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                                (u >= '0' && u <= '9') ||
                                u == '-' || u == '_' || u == '.' || u == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", u);
            out.append(buf, 3);
        }
    }
    return out;
}

}  // namespace

std::string synthesize_otpauth_uri(const std::string& login,
                                    const std::string& shared_secret_b64) {
    if (shared_secret_b64.empty()) return {};
    const auto raw = crypto::base64_decode(shared_secret_b64);
    if (raw.empty()) return {};
    std::string secret_b32 = base32_encode(
        std::span<const std::uint8_t>{raw.data(), raw.size()});
    while (!secret_b32.empty() && secret_b32.back() == '=') secret_b32.pop_back();

    std::string uri = "otpauth://totp/Steam:";
    uri += url_encode(login.empty() ? std::string{"account"} : login);
    uri += "?secret=";
    uri += secret_b32;
    uri += "&issuer=Steam&digits=5&period=30&algorithm=SHA1";
    return uri;
}

}  // namespace sam::sda
