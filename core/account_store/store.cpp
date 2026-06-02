#include "core/account_store/store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "core/account_store/schema.hpp"
#include "core/crypto/aead.hpp"
#include "core/crypto/kdf.hpp"
#include "core/crypto/rng.hpp"
#include "platform/fs.hpp"

namespace sam::core {

using json = nlohmann::json;

namespace {

std::string secure_to_string(const crypto::SecureString& s) {
    return std::string(s.begin(), s.end());
}

crypto::SecureString string_to_secure(const std::string& s) {
    crypto::SecureString r;
    r.assign(s.begin(), s.end());
    return r;
}

std::string ws_to_u8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring u8_to_ws(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                       nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace

// to_json / from_json must live in the same namespace as the type so nlohmann's
// ADL finds them. All of our domain types live in sam::core.

void to_json(json& j, const Tag& t) {
    j = json{{"id", t.id}, {"name", t.name}, {"color", t.color_rgba}};
}
void from_json(const json& j, Tag& t) {
    t.id = j.value("id", std::string{});
    t.name = j.value("name", std::string{});
    t.color_rgba = j.value("color", 0x4F8EF7FFu);
}

void to_json(json& j, const Group& g) {
    j = json{{"id", g.id}, {"name", g.name}, {"color", g.color_rgba}};
}
void from_json(const json& j, Group& g) {
    g.id = j.value("id", std::string{});
    g.name = j.value("name", std::string{});
    g.color_rgba = j.value("color", 0x4F8EF7FFu);
}

void to_json(json& j, const SteamGuardAccount& s) {
    j = json{
        {"shared_secret",  s.shared_secret},
        {"serial_number",  s.serial_number},
        {"revocation_code", s.revocation_code},
        {"uri",            s.uri},
        {"server_time",    s.server_time},
        {"account_name",   s.account_name},
        {"token_gid",      s.token_gid},
        {"identity_secret", s.identity_secret},
        {"secret_1",       s.secret_1},
        {"status",         s.status},
        {"device_id",      s.device_id},
        {"fully_enrolled", s.fully_enrolled},
    };
}
void from_json(const json& j, SteamGuardAccount& s) {
    s.shared_secret   = j.value("shared_secret", std::string{});
    s.serial_number   = j.value("serial_number", std::string{});
    s.revocation_code = j.value("revocation_code", std::string{});
    s.uri             = j.value("uri", std::string{});
    s.server_time     = j.value("server_time", static_cast<std::int64_t>(0));
    s.account_name    = j.value("account_name", std::string{});
    s.token_gid       = j.value("token_gid", std::string{});
    s.identity_secret = j.value("identity_secret", std::string{});
    s.secret_1        = j.value("secret_1", std::string{});
    s.status          = j.value("status", 1);
    s.device_id       = j.value("device_id", std::string{});
    s.fully_enrolled  = j.value("fully_enrolled", true);
}

void to_json(json& j, const WebProfile& w) {
    j = json{
        {"persona_name",          w.persona_name},
        {"profile_url",           w.profile_url},
        {"avatar_url_full",       w.avatar_url_full},
        {"country_code",          w.country_code},
        {"created_unix",          w.created_unix},
        {"community_visibility",  w.community_visibility_state},
        {"profile_state",         w.profile_state},
        {"steam_level",           w.steam_level},
        {"owned_games_count",     w.owned_games_count},
        {"total_playtime_minutes", w.total_playtime_minutes},
        {"last_refreshed_unix",   w.last_refreshed_unix},
    };
}
void from_json(const json& j, WebProfile& w) {
    w.persona_name              = j.value("persona_name", std::string{});
    w.profile_url               = j.value("profile_url", std::string{});
    w.avatar_url_full           = j.value("avatar_url_full", std::string{});
    w.country_code              = j.value("country_code", std::string{});
    w.created_unix              = j.value("created_unix", static_cast<std::int64_t>(0));
    w.community_visibility_state = j.value("community_visibility", 0);
    w.profile_state             = j.value("profile_state", 0);
    w.steam_level               = j.value("steam_level", -1);
    w.owned_games_count         = j.value("owned_games_count", -1);
    w.total_playtime_minutes    = j.value("total_playtime_minutes", static_cast<std::int64_t>(0));
    w.last_refreshed_unix       = j.value("last_refreshed_unix", static_cast<std::int64_t>(0));
}

void to_json(json& j, const BanStatus& b) {
    j = json{
        {"community_banned",     b.community_banned},
        {"vac_banned",           b.vac_banned},
        {"vac_ban_count",        b.vac_ban_count},
        {"game_ban_count",       b.game_ban_count},
        {"days_since_last_ban",  b.days_since_last_ban},
        {"economy_ban",          b.economy_ban},
        {"last_refreshed_unix",  b.last_refreshed_unix},
    };
}
void from_json(const json& j, BanStatus& b) {
    b.community_banned    = j.value("community_banned", false);
    b.vac_banned          = j.value("vac_banned", false);
    b.vac_ban_count       = j.value("vac_ban_count", 0);
    b.game_ban_count      = j.value("game_ban_count", 0);
    b.days_since_last_ban = j.value("days_since_last_ban", 0);
    b.economy_ban         = j.value("economy_ban", std::string{});
    b.last_refreshed_unix = j.value("last_refreshed_unix", static_cast<std::int64_t>(0));
}

void to_json(json& j, const CS2Status& c) {
    j = json{
        {"premier_rating",       c.premier_rating},
        {"premier_wins",         c.premier_wins},
        {"wingman_rank",         c.wingman_rank},
        {"wingman_wins",         c.wingman_wins},
        {"cs2_player_level",     c.cs2_player_level},
        {"cs2_player_xp",        c.cs2_player_xp},
        {"prime_status",         c.prime_status},
        {"vac_live",             c.vac_live},
        {"cooldown_expires_unix", c.cooldown_expires_unix},
        {"cooldown_reason",      c.cooldown_reason},
        {"last_refreshed_unix",  c.last_refreshed_unix},
    };
}
void to_json(json& j, const PreviousSnapshot& p) {
    j = json{
        {"vac_ban_count",         p.vac_ban_count},
        {"game_ban_count",        p.game_ban_count},
        {"community_banned",      p.community_banned},
        {"economy_ban",           p.economy_ban},
        {"vac_live",              p.vac_live},
        {"cooldown_expires_unix", p.cooldown_expires_unix},
        {"snapshot_unix",         p.snapshot_unix},
    };
}
void from_json(const json& j, PreviousSnapshot& p) {
    p.vac_ban_count         = j.value("vac_ban_count", 0);
    p.game_ban_count        = j.value("game_ban_count", 0);
    p.community_banned      = j.value("community_banned", false);
    p.economy_ban           = j.value("economy_ban", std::string{});
    p.vac_live              = j.value("vac_live", false);
    p.cooldown_expires_unix = j.value("cooldown_expires_unix", static_cast<std::int64_t>(0));
    p.snapshot_unix         = j.value("snapshot_unix", static_cast<std::int64_t>(0));
}

void from_json(const json& j, CS2Status& c) {
    c.premier_rating        = j.value("premier_rating", -1);
    c.premier_wins          = j.value("premier_wins", -1);
    c.wingman_rank          = j.value("wingman_rank", -1);
    c.wingman_wins          = j.value("wingman_wins", -1);
    c.cs2_player_level      = j.value("cs2_player_level", -1);
    c.cs2_player_xp         = j.value("cs2_player_xp", -1);
    c.prime_status          = j.value("prime_status", false);
    c.vac_live              = j.value("vac_live", false);
    c.cooldown_expires_unix = j.value("cooldown_expires_unix", static_cast<std::int64_t>(0));
    c.cooldown_reason       = j.value("cooldown_reason", std::string{});
    c.last_refreshed_unix   = j.value("last_refreshed_unix", static_cast<std::int64_t>(0));
}

void to_json(json& j, const Account& a) {
    j = json::object();
    j["id"]                   = a.id;
    j["steam_id_64"]          = a.steam_id_64;
    j["login"]                = a.login;
    j["password"]             = secure_to_string(a.password);
    if (a.sda.has_value()) j["sda"] = *a.sda;
    j["access_token"]         = secure_to_string(a.access_token);
    j["refresh_token"]        = secure_to_string(a.refresh_token);
    j["access_token_expires"] = a.access_token_expires;
    j["session_id"]           = a.session_id;
    j["steam_login_secure"]   = secure_to_string(a.steam_login_secure);
    j["is_nfa"]               = a.is_nfa;
    j["refresh_token_expires"] = a.refresh_token_expires;
    j["display_name"]         = ws_to_u8(a.display_name);
    j["notes"]                = ws_to_u8(a.notes);
    j["tag_ids"]              = a.tag_ids;
    j["group_id"]             = a.group_id;
    j["trust"]                = static_cast<int>(a.trust);
    if (a.trade_url.has_value()) j["trade_url"] = *a.trade_url;
    j["web"]                  = a.web;
    j["bans"]                 = a.bans;
    j["cs2"]                  = a.cs2;
    j["prev_snapshot"]        = a.prev_snapshot;
    j["created_unix"]         = a.created_unix;
    j["last_login_unix"]      = a.last_login_unix;
}

void from_json(const json& j, Account& a) {
    a.id                   = j.value("id", std::string{});
    a.steam_id_64          = j.value("steam_id_64", static_cast<std::uint64_t>(0));
    a.login                = j.value("login", std::string{});
    a.password             = string_to_secure(j.value("password", std::string{}));
    if (j.contains("sda") && !j["sda"].is_null()) {
        a.sda = j.at("sda").get<SteamGuardAccount>();
    }
    a.access_token         = string_to_secure(j.value("access_token", std::string{}));
    a.refresh_token        = string_to_secure(j.value("refresh_token", std::string{}));
    a.access_token_expires = j.value("access_token_expires", static_cast<std::int64_t>(0));
    a.session_id           = j.value("session_id", std::string{});
    a.steam_login_secure   = string_to_secure(j.value("steam_login_secure", std::string{}));
    a.is_nfa               = j.value("is_nfa", false);
    a.refresh_token_expires = j.value("refresh_token_expires", static_cast<std::int64_t>(0));
    a.display_name         = u8_to_ws(j.value("display_name", std::string{}));
    a.notes                = u8_to_ws(j.value("notes", std::string{}));
    if (j.contains("tag_ids") && j["tag_ids"].is_array()) {
        a.tag_ids = j.at("tag_ids").get<std::vector<std::string>>();
    }
    a.group_id = j.value("group_id", std::string{});
    a.trust = static_cast<TrustLabel>(j.value("trust", 0));
    if (j.contains("trade_url") && j["trade_url"].is_string()) {
        a.trade_url = j["trade_url"].get<std::string>();
    }
    if (j.contains("web"))  a.web  = j.at("web").get<WebProfile>();
    if (j.contains("bans")) a.bans = j.at("bans").get<BanStatus>();
    if (j.contains("cs2"))  a.cs2  = j.at("cs2").get<CS2Status>();
    if (j.contains("prev_snapshot")) {
        a.prev_snapshot = j.at("prev_snapshot").get<PreviousSnapshot>();
    }
    a.created_unix    = j.value("created_unix", static_cast<std::int64_t>(0));
    a.last_login_unix = j.value("last_login_unix", static_cast<std::int64_t>(0));
}

void to_json(json& j, const Vault& v) {
    j = json::object();
    j["schema_version"] = v.schema_version;
    j["tags"]           = v.tags;
    j["groups"]         = v.groups;
    j["accounts"]       = v.accounts;
}

void from_json(const json& j, Vault& v) {
    // Loaded version is informational only; the on-disk header version is
    // authoritative for compatibility checks. Bump to the current build so
    // the next save carries the latest schema.
    v.schema_version = static_cast<int>(store::kSchemaVersion);
    (void)j.value("schema_version", static_cast<int>(store::kSchemaVersion));
    if (j.contains("tags") && j["tags"].is_array()) {
        v.tags = j.at("tags").get<std::vector<Tag>>();
    }
    if (j.contains("groups") && j["groups"].is_array()) {
        v.groups = j.at("groups").get<std::vector<Group>>();
    }
    if (j.contains("accounts") && j["accounts"].is_array()) {
        v.accounts = j.at("accounts").get<std::vector<Account>>();
    }
}

}  // namespace sam::core


namespace sam::core::store {

using json = nlohmann::json;

namespace {

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("vault: open for read failed");
    }
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    f.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(buf.data()), size);
    }
    return buf;
}

std::vector<std::uint8_t> derive_key(const crypto::SecureString& password,
                                     std::span<const std::uint8_t> salt,
                                     std::uint32_t iterations) {
    std::span<const std::uint8_t> pw(
        reinterpret_cast<const std::uint8_t*>(password.data()), password.size());
    return crypto::pbkdf2(crypto::KdfHash::Sha256, pw, salt, iterations, kKeyLen);
}

std::vector<std::uint8_t> encrypt_vault(const Vault& vault,
                                        const crypto::SecureString& password) {
    json j = vault;
    const auto cbor = json::to_cbor(j);

    auto salt = crypto::random_bytes(kSaltLen);
    auto nonce = crypto::random_bytes(kNonceLen);
    auto key = derive_key(password, salt, kDefaultKdfIterations);

    const auto result = crypto::aes256_gcm_encrypt(
        key, nonce,
        std::span<const std::uint8_t>{},
        std::span<const std::uint8_t>{cbor.data(), cbor.size()});

    crypto::zero_buffer(key.data(), key.size());

    std::vector<std::uint8_t> file;
    file.reserve(kMagic.size() + 4 * 4 + salt.size() + nonce.size() + kTagLen +
                  result.ciphertext.size());
    file.insert(file.end(), kMagic.begin(), kMagic.end());
    write_u32(file, kSchemaVersion);
    write_u32(file, kKdfPbkdf2Sha256);
    write_u32(file, kDefaultKdfIterations);
    file.insert(file.end(), salt.begin(), salt.end());
    file.insert(file.end(), nonce.begin(), nonce.end());
    file.insert(file.end(), result.tag.begin(), result.tag.end());
    write_u32(file, static_cast<std::uint32_t>(result.ciphertext.size()));
    file.insert(file.end(), result.ciphertext.begin(), result.ciphertext.end());
    return file;
}

Vault decrypt_vault(const std::vector<std::uint8_t>& file,
                    const crypto::SecureString& password) {
    constexpr std::size_t header_min =
        kMagic.size() + 4 + 4 + 4 + kSaltLen + kNonceLen + kTagLen + 4;
    if (file.size() < header_min) {
        throw CorruptVault("vault: file truncated");
    }

    if (!std::equal(kMagic.begin(), kMagic.end(), file.begin())) {
        throw CorruptVault("vault: bad magic");
    }
    std::size_t off = kMagic.size();

    const std::uint32_t schema = read_u32(&file[off]); off += 4;
    if (schema > kSchemaVersion) {
        throw UnsupportedVersion("vault: schema_version newer than this build");
    }

    const std::uint32_t kdf_id = read_u32(&file[off]); off += 4;
    const std::uint32_t iterations = read_u32(&file[off]); off += 4;
    if (kdf_id != kKdfPbkdf2Sha256) {
        throw CorruptVault("vault: unknown kdf_id");
    }
    if (iterations == 0 || iterations > 10'000'000) {
        throw CorruptVault("vault: implausible iteration count");
    }

    std::span<const std::uint8_t> salt{&file[off], kSaltLen};       off += kSaltLen;
    std::span<const std::uint8_t> nonce{&file[off], kNonceLen};     off += kNonceLen;
    std::span<const std::uint8_t> tag{&file[off], kTagLen};         off += kTagLen;

    const std::uint32_t ct_len = read_u32(&file[off]); off += 4;
    if (off + ct_len != file.size()) {
        throw CorruptVault("vault: trailing bytes or truncated ciphertext");
    }
    std::span<const std::uint8_t> ciphertext{&file[off], ct_len};

    auto key = derive_key(password, salt, iterations);

    std::vector<std::uint8_t> plain;
    try {
        plain = crypto::aes256_gcm_decrypt(key, nonce, {}, ciphertext, tag);
    } catch (const crypto::AeadDecryptError&) {
        crypto::zero_buffer(key.data(), key.size());
        throw WrongPassword("vault: master password incorrect");
    }
    crypto::zero_buffer(key.data(), key.size());

    try {
        const json j = json::from_cbor(plain);
        Vault v = j.get<Vault>();
        return v;
    } catch (const std::exception& ex) {
        throw CorruptVault(std::string("vault: cbor decode failed: ") + ex.what());
    }
}

}  // namespace

bool vault_exists(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::array<char, 4> head{};
    f.read(head.data(), head.size());
    return f.gcount() == 4 && std::equal(kMagic.begin(), kMagic.end(),
                                          reinterpret_cast<std::uint8_t*>(head.data()));
}

void create_new_vault(const std::filesystem::path& path,
                      const crypto::SecureString& password) {
    Vault empty;
    save_vault(path, empty, password);
}

Vault load_vault(const std::filesystem::path& path,
                 const crypto::SecureString& password) {
    auto raw = read_file(path);
    return decrypt_vault(raw, password);
}

void save_vault(const std::filesystem::path& path,
                const Vault& vault,
                const crypto::SecureString& password) {
    const auto blob = encrypt_vault(vault, password);
    platform::atomic_write_file(path, blob);
}

void save_bundle(const std::filesystem::path& path,
                 const Vault& bundle,
                 const crypto::SecureString& passphrase) {
    save_vault(path, bundle, passphrase);
}

Vault load_bundle(const std::filesystem::path& path,
                  const crypto::SecureString& passphrase) {
    return load_vault(path, passphrase);
}

namespace {

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

// steam_id_64 first when both sides have one, otherwise case-insensitive login.
bool same_account(const Account& a, const Account& b) {
    if (a.steam_id_64 != 0 && b.steam_id_64 != 0) {
        return a.steam_id_64 == b.steam_id_64;
    }
    return iequals_ascii(a.login, b.login);
}

}  // namespace

Account* find_existing_account(Vault& vault,
                                std::uint64_t steam_id_64,
                                std::string_view login) {
    if (steam_id_64 != 0) {
        for (auto& acc : vault.accounts) {
            if (acc.steam_id_64 == steam_id_64) return &acc;
        }
    }
    if (!login.empty()) {
        for (auto& acc : vault.accounts) {
            if (iequals_ascii(acc.login, login)) return &acc;
        }
    }
    return nullptr;
}

namespace {
constexpr std::uint32_t kNfaColorRgba = 0xF5A623FFu;  // amber
}

std::string ensure_nfa_group(Vault& vault) {
    for (const auto& g : vault.groups) {
        if (g.id == "grp-nfa" || iequals_ascii(g.name, "NFA")) return g.id;
    }
    Group g;
    g.id = "grp-nfa";
    g.name = "NFA";
    g.color_rgba = kNfaColorRgba;
    vault.groups.push_back(g);
    return g.id;
}

Vault subset_vault(const Vault& source, std::span<const std::string> ids) {
    Vault out;
    out.schema_version = source.schema_version;

    const std::unordered_set<std::string> wanted(ids.begin(), ids.end());

    std::unordered_set<std::string> referenced_tag_ids;
    std::unordered_set<std::string> referenced_group_ids;
    out.accounts.reserve(wanted.size());
    for (const auto& a : source.accounts) {
        if (wanted.count(a.id) == 0) continue;
        out.accounts.push_back(a);
        for (const auto& tid : a.tag_ids) {
            referenced_tag_ids.insert(tid);
        }
        if (!a.group_id.empty()) {
            referenced_group_ids.insert(a.group_id);
        }
    }

    for (const auto& t : source.tags) {
        if (referenced_tag_ids.count(t.id) != 0) {
            out.tags.push_back(t);
        }
    }
    for (const auto& g : source.groups) {
        if (referenced_group_ids.count(g.id) != 0) {
            out.groups.push_back(g);
        }
    }
    return out;
}

MergeReport preview_merge(const Vault& dst, const Vault& imported) {
    MergeReport report;
    for (const auto& t : imported.tags) {
        const bool exists = std::any_of(dst.tags.begin(), dst.tags.end(),
            [&](const Tag& existing) { return existing.id == t.id; });
        if (!exists) ++report.tags_added;
    }
    for (const auto& incoming : imported.accounts) {
        auto match = std::find_if(dst.accounts.begin(), dst.accounts.end(),
            [&](const Account& existing) { return same_account(existing, incoming); });
        if (match != dst.accounts.end()) {
            ++report.overwritten;
            report.overwritten_logins.push_back(match->login);
        } else {
            ++report.added;
        }
    }
    return report;
}

MergeReport merge_into(Vault& dst, Vault imported) {
    MergeReport report;

    for (const auto& t : imported.tags) {
        const bool exists = std::any_of(dst.tags.begin(), dst.tags.end(),
            [&](const Tag& existing) { return existing.id == t.id; });
        if (!exists) {
            dst.tags.push_back(t);
            ++report.tags_added;
        }
    }

    for (const auto& g : imported.groups) {
        const bool exists = std::any_of(dst.groups.begin(), dst.groups.end(),
            [&](const Group& existing) { return existing.id == g.id; });
        if (!exists) {
            dst.groups.push_back(g);
        }
    }

    for (auto& incoming : imported.accounts) {
        auto match = std::find_if(dst.accounts.begin(), dst.accounts.end(),
            [&](const Account& existing) { return same_account(existing, incoming); });
        if (match != dst.accounts.end()) {
            // Preserve the destination ULID so existing references (in-flight
            // edits, refresh cooldown maps, selected_account_id) stay stable.
            std::string preserved_id = match->id;
            *match = std::move(incoming);
            match->id = std::move(preserved_id);
            ++report.overwritten;
            report.overwritten_logins.push_back(match->login);
        } else {
            dst.accounts.push_back(std::move(incoming));
            ++report.added;
        }
    }

    return report;
}

}  // namespace sam::core::store
