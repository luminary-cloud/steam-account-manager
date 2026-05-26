#include "core/sda/mafile.hpp"

#include <fstream>
#include <span>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/crypto/aes_cbc.hpp"
#include "core/crypto/base64.hpp"
#include "core/crypto/kdf.hpp"

namespace sam::sda {

using json = nlohmann::json;

namespace {

constexpr std::uint32_t kSdaPbkdfIterationsDefault = 50000;
constexpr std::size_t kSdaKeyLen = 32;
constexpr std::size_t kSdaIvLen = 16;
constexpr std::size_t kSdaSaltLen = 8;

bool looks_like_encrypted(const json& j) {
    return j.contains("encrypted") && j["encrypted"].is_string() && !j["encrypted"].get<std::string>().empty();
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw MafileError("mafile: could not open file");
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

core::SteamGuardAccount from_json_obj(const json& j) {
    core::SteamGuardAccount s;
    s.shared_secret   = j.value("shared_secret", "");
    s.serial_number   = j.value("serial_number", "");
    s.revocation_code = j.value("revocation_code", "");
    s.uri             = j.value("uri", "");
    s.server_time     = j.value("server_time", 0);
    s.account_name    = j.value("account_name", "");
    s.token_gid       = j.value("token_gid", "");
    s.identity_secret = j.value("identity_secret", "");
    s.secret_1        = j.value("secret_1", "");
    s.status          = j.value("status", 1);
    s.device_id       = j.value("device_id", "");
    s.fully_enrolled  = j.value("fully_enrolled", true);
    return s;
}

std::uint64_t parse_json_u64(const json& v) {
    if (v.is_number_unsigned())          return v.get<std::uint64_t>();
    if (v.is_number_integer())           return static_cast<std::uint64_t>(v.get<std::int64_t>());
    if (v.is_number_float())             return static_cast<std::uint64_t>(v.get<double>());
    if (v.is_string()) {
        try { return std::stoull(v.get<std::string>()); } catch (...) { return 0; }
    }
    return 0;
}

void extract_session(const json& j, MafileLoadResult& out) {
    if (!j.contains("Session") || !j["Session"].is_object()) return;
    const auto& s = j["Session"];
    if (s.contains("SteamID"))      out.session_steam_id      = parse_json_u64(s["SteamID"]);
    if (s.contains("AccessToken"))  out.session_access_token  = s.value("AccessToken",  "");
    if (s.contains("RefreshToken")) out.session_refresh_token = s.value("RefreshToken", "");
}

std::vector<std::uint8_t> derive_sda_key(const crypto::SecureString& password,
                                          std::span<const std::uint8_t> salt,
                                          std::uint32_t iterations) {
    std::span<const std::uint8_t> pw(
        reinterpret_cast<const std::uint8_t*>(password.data()), password.size());
    return crypto::pbkdf2(crypto::KdfHash::Sha1, pw, salt, iterations, kSdaKeyLen);
}

}  // namespace

MafileLoadResult parse_mafile_json(std::string_view json_text) {
    json j;
    try {
        j = json::parse(json_text);
    } catch (const std::exception& ex) {
        throw MafileError(std::string{"mafile: malformed JSON: "} + ex.what());
    }

    if (looks_like_encrypted(j)) {
        throw MafileEncrypted("mafile: encrypted (call load_mafile with the password)");
    }
    MafileLoadResult out;
    out.guard = from_json_obj(j);
    extract_session(j, out);
    return out;
}

MafileLoadResult load_mafile(const std::filesystem::path& path,
                             const crypto::SecureString& password) {
    const std::string text = read_text(path);

    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& ex) {
        throw MafileError(std::string{"mafile: malformed JSON: "} + ex.what());
    }

    if (!looks_like_encrypted(j)) {
        MafileLoadResult out;
        out.guard = from_json_obj(j);
        extract_session(j, out);
        return out;
    }

    if (password.empty()) {
        throw MafileEncrypted("mafile: password required");
    }

    const std::string ct_b64   = j.value("encrypted", "");
    const std::string iv_b64   = j.value("encryption_iv", "");
    const std::string salt_b64 = j.value("encryption_salt", "");
    const std::uint32_t iters  = j.value("Encryption_Iterations", kSdaPbkdfIterationsDefault);

    const auto ct   = crypto::base64_decode(ct_b64);
    const auto iv   = crypto::base64_decode(iv_b64);
    const auto salt = crypto::base64_decode(salt_b64);

    if (iv.size() != kSdaIvLen) {
        throw MafileError("mafile: encryption_iv must decode to 16 bytes");
    }
    if (salt.size() != kSdaSaltLen) {
        throw MafileError("mafile: encryption_salt must decode to 8 bytes");
    }
    if (ct.empty() || (ct.size() % 16) != 0) {
        throw MafileError("mafile: encrypted blob has invalid size");
    }

    auto key = derive_sda_key(password,
                              std::span<const std::uint8_t>{salt.data(), salt.size()},
                              iters);

    std::vector<std::uint8_t> plain;
    try {
        plain = crypto::aes256_cbc_pkcs7_decrypt(
            std::span<const std::uint8_t>{key.data(), key.size()},
            std::span<const std::uint8_t>{iv.data(), iv.size()},
            std::span<const std::uint8_t>{ct.data(), ct.size()});
    } catch (const crypto::CbcPaddingError&) {
        crypto::zero_buffer(key.data(), key.size());
        throw MafileWrongPassword("mafile: wrong password");
    }
    crypto::zero_buffer(key.data(), key.size());

    json inner;
    try {
        inner = json::parse(std::string_view{
            reinterpret_cast<const char*>(plain.data()), plain.size()});
    } catch (const std::exception& ex) {
        throw MafileError(std::string{"mafile: decrypted payload not JSON: "} + ex.what());
    }
    MafileLoadResult out;
    out.guard = from_json_obj(inner);
    extract_session(inner, out);
    return out;
}

}  // namespace sam::sda
