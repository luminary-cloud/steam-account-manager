#include "core/steam_login/mobile_auth.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "core/sda/totp.hpp"
#include "core/time_aligner.hpp"

#include "core/crypto/base64.hpp"
#include "core/crypto/rng.hpp"
#include "core/http/client.hpp"
#include "core/http/url.hpp"
#include "core/log.hpp"
#include "core/steam_login/rsa_password.hpp"
#include "core/steam_login/session.hpp"

namespace sam::steam_login {

namespace {

// Tiny protobuf encoder/decoder. We only need varint and length-delimited
// fields; Steam's auth messages don't use packed repeated or fixed32/64 in the
// surface we touch.

constexpr int kWireVarint   = 0;
constexpr int kWireFixed64  = 1;
constexpr int kWireLenDelim = 2;

void enc_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void enc_tag(std::vector<std::uint8_t>& out, int field, int wire) {
    enc_varint(out, (static_cast<std::uint64_t>(field) << 3) |
                        static_cast<std::uint64_t>(wire));
}

void enc_string(std::vector<std::uint8_t>& out, int field, std::string_view s) {
    enc_tag(out, field, kWireLenDelim);
    enc_varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

void enc_bytes(std::vector<std::uint8_t>& out, int field,
               std::span<const std::uint8_t> b) {
    enc_tag(out, field, kWireLenDelim);
    enc_varint(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

void enc_message(std::vector<std::uint8_t>& out, int field,
                 const std::vector<std::uint8_t>& inner) {
    enc_tag(out, field, kWireLenDelim);
    enc_varint(out, inner.size());
    out.insert(out.end(), inner.begin(), inner.end());
}

void enc_uint64(std::vector<std::uint8_t>& out, int field, std::uint64_t v) {
    enc_tag(out, field, kWireVarint);
    enc_varint(out, v);
}

// Protobuf `fixed64`: 8 bytes little-endian, wire type 1.
void enc_fixed64(std::vector<std::uint8_t>& out, int field, std::uint64_t v) {
    enc_tag(out, field, kWireFixed64);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

void enc_int32(std::vector<std::uint8_t>& out, int field, std::int32_t v) {
    enc_tag(out, field, kWireVarint);
    enc_varint(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
}

void enc_bool(std::vector<std::uint8_t>& out, int field, bool v) {
    enc_tag(out, field, kWireVarint);
    enc_varint(out, v ? 1 : 0);
}

struct Reader {
    const std::uint8_t* p;
    const std::uint8_t* end;
    bool ok = true;

    bool eof() const { return p >= end || !ok; }

    std::uint64_t varint() {
        std::uint64_t v = 0;
        int shift = 0;
        while (p < end) {
            const std::uint8_t b = *p++;
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
            shift += 7;
            if (shift > 63) {
                ok = false;
                return 0;
            }
        }
        ok = false;
        return 0;
    }

    std::string_view lendelim() {
        const std::uint64_t n = varint();
        if (!ok || (p + n) > end) {
            ok = false;
            return {};
        }
        std::string_view sv(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
        p += n;
        return sv;
    }

    void skip(int wire) {
        if (wire == kWireVarint) {
            (void)varint();
        } else if (wire == kWireLenDelim) {
            (void)lendelim();
        } else {
            ok = false;
        }
    }
};

const char* eresult_label(std::string_view er) {
    if (er == "5")  return "(InvalidPassword)";
    if (er == "11") return "(InvalidProtocolVer)";
    if (er == "15") return "(AccessDenied)";
    if (er == "17") return "(Banned)";
    if (er == "18") return "(AccountNotFound)";
    if (er == "20") return "(PasswordRequiredToKickSession)";
    if (er == "63") return "(AccountLogonDeniedNeedTwoFactor)";
    if (er == "65") return "(TwoFactorCodeMismatch)";
    if (er == "84") return "(RateLimitExceeded)";
    if (er == "85") return "(AccountLoginDeniedNeedTwoFactor)";
    return "";
}

std::string format_post_error(const std::string& fallback, const std::string& er) {
    if (!er.empty() && er != "1") {
        std::string msg = "Steam EResult=" + er;
        const char* label = eresult_label(er);
        if (*label) { msg.push_back(' '); msg += label; }
        return msg;
    }
    return fallback;
}

// Returns nullopt on transport/HTTP failure or any Steam-side x-eresult error.
// Writes the raw x-eresult value into *eresult_out when non-null, so callers
// can distinguish "Steam rejected us" from "couldn't reach Steam".
std::optional<std::string> post_protobuf(const std::string& path,
                                          const std::vector<std::uint8_t>& msg,
                                          const std::string& access_token = "",
                                          std::string* eresult_out = nullptr) {
    http::Request req;
    req.method = http::Method::Post;
    req.url = "https://api.steampowered.com" + path;
    if (!access_token.empty()) {
        req.url += "?access_token=" + http::url_encode(access_token);
    }

    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    // Browser-shape headers: Steam's IAuthenticationService endpoints
    // rate-limit non-browser-looking clients aggressively (5/15min/IP).
    req.headers["Origin"]  = "https://steamcommunity.com";
    req.headers["Referer"] = "https://steamcommunity.com/login/";
    req.headers["Accept"]  = "*/*";
    req.headers["Accept-Language"] = "en-US,en;q=0.9";
    req.user_agent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
    req.body = "input_protobuf_encoded=" +
               crypto::base64_url_query_encode(crypto::base64_encode(
                   std::span<const std::uint8_t>{msg.data(), msg.size()}));

    auto resp = http::request(req);
    const auto er_it = resp.headers.find("x-eresult");
    const std::string er = (er_it != resp.headers.end()) ? er_it->second : std::string{};
    if (eresult_out) *eresult_out = er;
    SAM_LOG_INFO("auth: {} http {} ({} bytes, x-eresult='{}')",
                 path, resp.status, resp.body.size(), er);
    if (resp.status != 200) return std::nullopt;
    if (!er.empty() && er != "1") return std::nullopt;
    return resp.body;
}

// Wire values per EAuthSessionGuardType in steammessages_auth.steamclient.proto:
// None=1, EmailCode=2, DeviceCode=3, DeviceConfirmation=4, EmailConfirmation=5.
constexpr std::array<std::pair<int, GuardKind>, 5> kGuardKindTable{{
    {1, GuardKind::None},
    {2, GuardKind::EmailCode},
    {3, GuardKind::DeviceCode},
    {4, GuardKind::DeviceConfirmation},
    {5, GuardKind::EmailConfirmation},
}};

GuardKind guard_kind_from_proto(int t) {
    for (const auto& [proto, kind] : kGuardKindTable) {
        if (proto == t) return kind;
    }
    return GuardKind::None;
}

int guard_kind_to_proto(GuardKind k) {
    for (const auto& [proto, kind] : kGuardKindTable) {
        if (kind == k) return proto;
    }
    return 0;
}

}  // namespace

BeginSessionResult begin_session(const MobileLogin& login) {
    BeginSessionResult out;

    const auto rsa = fetch_rsa_key(login.username);
    if (!rsa) {
        out.error = "failed to fetch RSA key";
        return out;
    }
    const std::string password(login.password.begin(), login.password.end());
    const std::string encrypted = encrypt_password(password, *rsa);
    if (encrypted.empty()) {
        out.error = "RSA encrypt failed";
        return out;
    }

    // device_details (field 9) gives Steam the metadata needed to issue a
    // session with full mobileconf write capability. Without it, the session
    // can list confirmations but accept/reject return `{"success":false}`.
    std::vector<std::uint8_t> device;
    enc_string(device, 1, login.device_friendly_name);
    enc_int32 (device, 2, 3);               // platform_type = MobileApp
    enc_int32 (device, 3, -500);            // os_type = Android

    std::vector<std::uint8_t> msg;
    enc_string (msg, 1, login.device_friendly_name);
    enc_string (msg, 2, login.username);
    enc_string (msg, 3, encrypted);
    enc_uint64 (msg, 4, rsa->timestamp);
    enc_bool   (msg, 5, true);              // remember_login (deprecated, harmless)
    enc_int32  (msg, 6, 3);                 // platform_type = MobileApp
    enc_int32  (msg, 7, 1);                 // persistence = Persistent
    // website_id "Mobile" scopes the access_token's `aud` to web:mobile,
    // which is the only audience /mobileconf/* accepts. Trade-off:
    // GenerateAccessTokenForApp returns x-eresult=15 for the resulting
    // refresh_token, so refresh_access_token always falls through to a
    // full auto_relogin. The 5-minute per-account cooldown in
    // AppState::auto_relogin keeps that from hammering Steam.
    enc_string (msg, 8, "Mobile");
    enc_message(msg, 9, device);
    enc_int32  (msg, 11, 0);                // language

    std::string er;
    const auto body_opt = post_protobuf(
        "/IAuthenticationService/BeginAuthSessionViaCredentials/v1/", msg, "", &er);
    if (!body_opt) {
        out.error = format_post_error(
            "BeginAuthSessionViaCredentials transport/HTTP failure", er);
        return out;
    }
    const std::string& body = *body_opt;
    if (body.empty()) {
        out.error = "BeginAuthSessionViaCredentials returned empty body (likely bad credentials)";
        return out;
    }

    Reader r{reinterpret_cast<const std::uint8_t*>(body.data()),
             reinterpret_cast<const std::uint8_t*>(body.data() + body.size())};
    while (!r.eof()) {
        const std::uint64_t tag = r.varint();
        if (!r.ok) break;
        const int field = static_cast<int>(tag >> 3);
        const int wire  = static_cast<int>(tag & 0x07);
        switch (field) {
            case 1:  out.client_id = std::to_string(r.varint()); break;
            case 2:  out.request_id = std::string(r.lendelim()); break;
            case 3: { // float interval, wire-type 5 (fixed32 IEEE-754).
                if (wire == 5) {
                    if (r.p + 4 > r.end) { r.ok = false; break; }
                    std::uint32_t bits = 0;
                    std::memcpy(&bits, r.p, 4);
                    r.p += 4;
                    float f = 0.0F;
                    std::memcpy(&f, &bits, 4);
                    out.interval_seconds = static_cast<std::int64_t>(f > 0.0F ? f : 5.0F);
                } else {
                    out.interval_seconds = static_cast<std::int64_t>(r.varint());
                }
                break;
            }
            case 4: {
                const auto inner_sv = r.lendelim();
                Reader ir{reinterpret_cast<const std::uint8_t*>(inner_sv.data()),
                          reinterpret_cast<const std::uint8_t*>(inner_sv.data() + inner_sv.size())};
                while (!ir.eof()) {
                    const std::uint64_t itag = ir.varint();
                    if (!ir.ok) break;
                    const int ifield = static_cast<int>(itag >> 3);
                    const int iwire  = static_cast<int>(itag & 0x07);
                    if (ifield == 1 && iwire == kWireVarint) {
                        const auto k = guard_kind_from_proto(static_cast<int>(ir.varint()));
                        out.allowed_confirmations.push_back(k);
                    } else {
                        ir.skip(iwire);
                    }
                }
                break;
            }
            case 5:  out.steam_id = r.varint(); break;
            case 6:  (void)r.lendelim(); break;
            default: r.skip(wire); break;
        }
    }

    out.ok = !out.client_id.empty() && out.steam_id != 0;
    if (!out.ok && out.error.empty()) out.error = "begin: malformed response";
    return out;
}

bool submit_guard_code(const std::string& client_id,
                        std::uint64_t steam_id,
                        const std::string& code,
                        GuardKind kind,
                        std::string* error_out) {
    if (client_id.empty() || steam_id == 0 || code.empty()) {
        if (error_out) *error_out = "missing parameters";
        return false;
    }

    std::vector<std::uint8_t> msg;
    enc_uint64 (msg, 1, std::stoull(client_id));
    enc_fixed64(msg, 2, steam_id);                  // proto: fixed64 steamid
    enc_string (msg, 3, code);
    enc_int32  (msg, 4, guard_kind_to_proto(kind));

    std::string er;
    const auto body_opt = post_protobuf(
        "/IAuthenticationService/UpdateAuthSessionWithSteamGuardCode/v1/", msg, "", &er);
    // Success response is an empty CResponseMessage: empty body with HTTP
    // 200 and x-eresult absent/1 is the normal "code accepted" signal.
    if (!body_opt) {
        if (error_out) *error_out = format_post_error(
            "UpdateAuthSessionWithSteamGuardCode rejected (HTTP/eresult error)", er);
        return false;
    }
    return true;
}

PollResult poll_session(const std::string& client_id, const std::string& request_id) {
    PollResult out;
    if (client_id.empty() || request_id.empty()) {
        out.error = "missing client_id or request_id";
        return out;
    }

    std::vector<std::uint8_t> msg;
    enc_uint64(msg, 1, std::stoull(client_id));
    enc_bytes(msg, 2,
              std::span<const std::uint8_t>{
                  reinterpret_cast<const std::uint8_t*>(request_id.data()),
                  request_id.size()});

    std::string er;
    const auto body_opt = post_protobuf(
        "/IAuthenticationService/PollAuthSessionStatus/v1/", msg, "", &er);
    if (!body_opt) {
        out.error = format_post_error("PollAuthSessionStatus transport/HTTP failure", er);
        return out;
    }
    const std::string& body = *body_opt;
    // An empty body on poll means "no progress yet"; caller should keep polling.

    Reader r{reinterpret_cast<const std::uint8_t*>(body.data()),
             reinterpret_cast<const std::uint8_t*>(body.data() + body.size())};
    while (!r.eof()) {
        const std::uint64_t tag = r.varint();
        if (!r.ok) break;
        const int field = static_cast<int>(tag >> 3);
        const int wire  = static_cast<int>(tag & 0x07);
        switch (field) {
            case 1: out.new_client_id = std::to_string(r.varint()); break;
            case 2: (void)r.lendelim(); break;
            case 3: out.refresh_token = crypto::make_secure(std::string(r.lendelim())); break;
            case 4: out.access_token  = crypto::make_secure(std::string(r.lendelim())); break;
            case 5: (void)r.varint(); break;
            case 6: (void)r.lendelim(); break;
            case 7: (void)r.lendelim(); break;
            case 8: (void)r.lendelim(); break;
            default: r.skip(wire); break;
        }
    }

    if (!out.access_token.empty() && !out.refresh_token.empty()) {
        out.finished = true;
        out.ok = true;
    } else {
        out.ok = true;  // No tokens yet but the call succeeded; caller should keep polling.
    }
    return out;
}

FullLoginResult run_full_login(
    const MobileLogin& login,
    const std::function<std::string(const std::vector<GuardKind>&)>& on_guard_needed,
    const std::function<void(const std::string&)>& on_status) {

    FullLoginResult result;
    if (on_status) on_status("requesting RSA key");

    auto begin = begin_session(login);
    if (!begin.ok) {
        result.error = begin.error;
        return result;
    }

    // If a guard code is required, ask the caller.
    GuardKind chosen = GuardKind::None;
    for (auto k : begin.allowed_confirmations) {
        if (k == GuardKind::DeviceCode || k == GuardKind::EmailCode) {
            chosen = k;
            break;
        }
    }
    if (chosen != GuardKind::None) {
        if (on_status) on_status("waiting for Steam Guard code");
        const std::string code = on_guard_needed ? on_guard_needed(begin.allowed_confirmations) : "";
        if (code.empty()) {
            result.error = "guard code not provided";
            return result;
        }
        std::string err;
        if (!submit_guard_code(begin.client_id, begin.steam_id, code, chosen, &err)) {
            result.error = "guard rejected: " + err;
            return result;
        }
    } else if (!begin.allowed_confirmations.empty() && on_status) {
        on_status("waiting for confirmation in the Steam Mobile app");
    }

    if (on_status) on_status("polling for tokens");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto poll = poll_session(begin.client_id, begin.request_id);
        if (!poll.ok) {
            result.error = poll.error;
            return result;
        }
        if (poll.finished) {
            result.account.steam_id_64 = begin.steam_id;
            result.account.login = login.username;
            result.account.password = login.password;
            result.account.access_token = std::move(poll.access_token);
            result.account.refresh_token = std::move(poll.refresh_token);
            result.account.access_token_expires = jwt_expiry(result.account.access_token);
            // Manual cookie is the fallback for /getlist if settoken below
            // fails. /ajaxop needs the settoken-minted value, so the next
            // block tries to upgrade.
            result.account.steam_login_secure = crypto::make_secure(
                make_steam_login_secure(begin.steam_id, result.account.access_token));
            result.account.session_id = crypto::random_session_id();
            // finalizelogin + settoken registers the session in Steam's
            // community session table and mints the per-domain
            // steamLoginSecure cookie that /mobileconf/ajaxop reads. Skip
            // silently on failure: callers still get a usable getlist
            // cookie above and a later refresh/relogin will retry.
            if (on_status) on_status("registering community session");
            std::string registered_cookie;
            if (transfer_login(begin.steam_id,
                               result.account.refresh_token,
                               result.account.session_id,
                               registered_cookie)) {
                result.account.steam_login_secure = crypto::make_secure(registered_cookie);
            } else if (on_status) {
                on_status("settoken failed; mobileconf writes may need a relogin");
            }
            result.ok = true;
            return result;
        }
        if (!poll.new_client_id.empty()) {
            begin.client_id = poll.new_client_id;
        }
        std::this_thread::sleep_for(std::chrono::seconds(begin.interval_seconds > 0
                                                             ? begin.interval_seconds
                                                             : 5));
    }

    result.error = "login timed out";
    return result;
}

std::string default_guard_provider(
    const std::optional<core::SteamGuardAccount>& sda,
    const std::vector<GuardKind>& allowed,
    const std::function<std::string(const std::vector<GuardKind>&)>& on_prompt) {

    const bool wants_device_code =
        std::find(allowed.begin(), allowed.end(), GuardKind::DeviceCode) != allowed.end();

    if (wants_device_code && sda.has_value() && !sda->shared_secret.empty()) {
        if (!time_aligner::synced()) {
            // Force one sync attempt; codes generated off an unsynced clock
            // are rejected with EResult 88 (TwoFactorCodeMismatch).
            (void)time_aligner::sync_now();
        }
        const std::string code = sda::generate_code_now(sda->shared_secret);
        if (!code.empty()) {
            SAM_LOG_INFO("auth: submitted Steam Guard code (no prompt)");
            return code;
        }
        SAM_LOG_WARN("auth: shared_secret present but code generation failed; falling back to prompt");
    }

    if (on_prompt) return on_prompt(allowed);
    return {};
}

}  // namespace sam::steam_login
