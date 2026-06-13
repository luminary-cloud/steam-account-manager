#include "core/sda/info_dat.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/crypto/aes_cbc.hpp"
#include "core/crypto/base64.hpp"
#include "core/crypto/kdf.hpp"
#include "core/crypto/rijndael256.hpp"

namespace sam::sda {

namespace {

constexpr std::size_t kSaltLen = 32;
constexpr std::size_t kIvLen   = 32;
constexpr std::size_t kKeyLen  = 32;
constexpr std::uint32_t kPbkdf2Iterations = 1000;

std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw InfoDatError("info.dat: open failed");
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// info.dat is either plain XML or `Base64( salt[32] | iv[32] | ct )`. The XML
// always starts with '<' (optionally preceded by a UTF-8 BOM). Base64 of
// random salt+iv almost never starts with '<'. Use the first non-whitespace
// byte to discriminate.
bool looks_like_xml(std::string_view text) {
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (static_cast<unsigned char>(c) == 0xEF) return true;  // UTF-8 BOM
        return c == '<';
    }
    return false;
}

// Strip whitespace from both ends of a Base64 envelope before passing it on.
std::string_view trim_view(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' ||
                          s.back()  == '\r' || s.back()  == '\n')) s.remove_suffix(1);
    return s;
}

// Tiny XML extractor specialised for .NET XmlSerializer output.

void skip_ws(std::string_view s, std::size_t& pos) {
    while (pos < s.size()) {
        const char c = s[pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++pos;
    }
}

// Returns the inner text of the first `<tag>...</tag>` found inside `block`,
// with XML entities decoded. Returns empty string if not found. Treats
// `<tag xsi:nil="true" />` and `<tag />` as empty.
std::string find_field(std::string_view block, std::string_view tag) {
    // Match `<tag>` or `<tag ` or `<tag/>`.
    std::size_t pos = 0;
    while (pos < block.size()) {
        const auto open = block.find('<', pos);
        if (open == std::string_view::npos) return {};
        if (open + 1 + tag.size() >= block.size()) return {};
        if (block.compare(open + 1, tag.size(), tag) != 0) {
            pos = open + 1;
            continue;
        }
        const char after = block[open + 1 + tag.size()];
        if (after != '>' && after != ' ' && after != '/' && after != '\t' &&
            after != '\r' && after != '\n') {
            // Tag starts with `<tag` but is actually a different element
            // (e.g. <SharedSecretRef>).
            pos = open + 1;
            continue;
        }
        // Found the opening of an element matching `tag`.
        std::size_t tag_end = open + 1 + tag.size();
        // Self-closing form: `<tag ... />` -> empty value.
        std::size_t close_bracket = block.find('>', tag_end);
        if (close_bracket == std::string_view::npos) return {};
        if (close_bracket > 0 && block[close_bracket - 1] == '/') return {};
        const std::size_t value_start = close_bracket + 1;
        // Look for the matching `</tag>`.
        std::string close = "</";
        close.append(tag);
        close.push_back('>');
        const std::size_t value_end = block.find(close, value_start);
        if (value_end == std::string_view::npos) return {};
        std::string_view raw = block.substr(value_start, value_end - value_start);

        // Decode entities.
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') { out.push_back(raw[i]); continue; }
            const auto semi = raw.find(';', i + 1);
            if (semi == std::string_view::npos) { out.push_back('&'); continue; }
            std::string_view ent = raw.substr(i + 1, semi - i - 1);
            if      (ent == "amp")   out.push_back('&');
            else if (ent == "lt")    out.push_back('<');
            else if (ent == "gt")    out.push_back('>');
            else if (ent == "quot")  out.push_back('"');
            else if (ent == "apos")  out.push_back('\'');
            else if (ent.size() > 1 && ent[0] == '#') {
                // Numeric entity.
                int base = 10;
                std::size_t off = 1;
                if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                    base = 16;
                    off = 2;
                }
                int code = 0;
                bool ok = !ent.substr(off).empty();
                for (char c : ent.substr(off)) {
                    int d = -1;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (base == 16 && c >= 'a' && c <= 'f') d = 10 + c - 'a';
                    else if (base == 16 && c >= 'A' && c <= 'F') d = 10 + c - 'A';
                    if (d < 0 || d >= base) { ok = false; break; }
                    code = code * base + d;
                }
                if (ok && code >= 0 && code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (ok) {
                    // UTF-8 encode (BMP-only is enough for this format).
                    if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                } else {
                    out.append("&").append(ent).append(";");
                }
            } else {
                out.append("&").append(ent).append(";");
            }
            i = semi;
        }
        return out;
    }
    return {};
}

bool to_bool(std::string_view s) {
    std::string lower(s);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower == "true" || lower == "1";
}

int to_int(std::string_view s) {
    try { return std::stoi(std::string(s)); }
    catch (...) { return 0; }
}

}  // namespace

namespace {

// Attempts to decrypt a StringCipher envelope under the given password.
// Returns std::nullopt if the input is not envelope-shaped or PKCS#7 unpad
// fails. Internal helper: public callers use try_unwrap_field() which walks
// the user key plus the known-key try-list.
std::optional<std::string> try_decrypt_string_cipher(std::string_view base64_envelope,
                                                      std::string_view password) {
    base64_envelope = trim_view(base64_envelope);
    if (base64_envelope.empty()) return std::nullopt;

    std::vector<std::uint8_t> raw;
    try {
        raw = crypto::base64_decode(base64_envelope);
    } catch (...) {
        return std::nullopt;
    }
    if (raw.empty()) return std::nullopt;

    // Envelope is salt(32) + iv(32) + ciphertext (multiple of the
    // Rijndael-256 block, i.e. 32 bytes).
    if (raw.size() < kSaltLen + kIvLen + 32) return std::nullopt;
    if ((raw.size() - kSaltLen - kIvLen) % 32 != 0) return std::nullopt;

    std::span<const std::uint8_t> salt{raw.data(), kSaltLen};
    std::span<const std::uint8_t> iv{raw.data() + kSaltLen, kIvLen};
    std::span<const std::uint8_t> ct{raw.data() + kSaltLen + kIvLen,
                                      raw.size() - kSaltLen - kIvLen};

    std::span<const std::uint8_t> pw{
        reinterpret_cast<const std::uint8_t*>(password.data()), password.size()};
    std::vector<std::uint8_t> key;
    try {
        key = crypto::pbkdf2(crypto::KdfHash::Sha1, pw, salt,
                              kPbkdf2Iterations, kKeyLen);
    } catch (...) {
        return std::nullopt;
    }

    try {
        auto plain = crypto::rijndael256_cbc_pkcs7_decrypt(key, iv, ct);
        return std::string(plain.begin(), plain.end());
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

std::string try_unwrap_field(std::string_view value, std::string_view user_key) {
    if (value.empty()) return std::string(value);

    // User-supplied key wins.
    if (!user_key.empty()) {
        if (auto plain = try_decrypt_string_cipher(value, user_key))
            return std::move(*plain);
    }
    // Fall back to the built-in list of known eKeys.
    for (const auto& k : kKnownInfoDatKeys) {
        if (auto plain = try_decrypt_string_cipher(value, k))
            return std::move(*plain);
    }
    return std::string(value);
}

std::vector<InfoDatAccount> parse_info_dat_xml(std::string_view xml,
                                                std::string_view field_key) {
    // Iterate <Account>...</Account> blocks.
    std::vector<InfoDatAccount> out;
    std::size_t pos = 0;
    while (true) {
        const auto open = xml.find("<Account>", pos);
        if (open == std::string_view::npos) break;
        const auto close = xml.find("</Account>", open);
        if (close == std::string_view::npos) break;
        std::string_view block = xml.substr(open, close - open);
        pos = close + std::strlen("</Account>");

        InfoDatAccount a;
        a.name             = find_field(block, "Name");
        a.alias            = find_field(block, "Alias");
        a.password         = try_unwrap_field(find_field(block, "Password"), field_key);
        a.shared_secret    = try_unwrap_field(find_field(block, "SharedSecret"), field_key);
        a.profile_url      = find_field(block, "ProfUrl");
        a.avatar_url       = find_field(block, "AviUrl");
        a.steam_id_str     = find_field(block, "SteamId");
        a.description      = find_field(block, "Description");
        a.community_banned = to_bool(find_field(block, "CommunityBanned"));
        a.vac_banned       = to_bool(find_field(block, "VACBanned"));
        a.vac_ban_count    = to_int (find_field(block, "NumberOfVACBans"));
        a.days_since_last_ban = to_int(find_field(block, "DaysSinceLastBan"));
        a.game_ban_count   = to_int (find_field(block, "NumberOfGameBans"));
        a.economy_ban      = find_field(block, "EconomyBan");

        // Skip totally-empty entries that XmlSerializer sometimes leaves
        // behind for nil/default rows.
        if (!a.name.empty() || !a.password.empty() || !a.shared_secret.empty()) {
            out.push_back(std::move(a));
        }
    }

    if (out.empty()) {
        throw InfoDatError("info.dat: no <Account> entries found in XML");
    }
    return out;
}

std::vector<InfoDatAccount> load_info_dat(const std::filesystem::path& path,
                                          const crypto::SecureString& file_password,
                                          std::string_view field_key) {
    std::string text = read_file_text(path);
    if (text.empty()) throw InfoDatError("info.dat: file is empty");

    if (looks_like_xml(text)) {
        return parse_info_dat_xml(text, field_key);
    }

    if (file_password.empty()) {
        throw InfoDatEncrypted("info.dat: file is encrypted but no password was supplied");
    }

    std::string_view trimmed = trim_view(text);
    std::string_view pw_view{file_password.data(), file_password.size()};
    auto xml = try_decrypt_string_cipher(trimmed, pw_view);
    if (!xml.has_value()) {
        throw InfoDatWrongPassword("info.dat: wrong password");
    }
    return parse_info_dat_xml(*xml, field_key);
}

}  // namespace sam::sda
