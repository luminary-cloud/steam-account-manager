#include "core/sda/confirmation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

#include "core/crypto/base64.hpp"
#include "core/crypto/hmac.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/http/client.hpp"
#include "core/http/url.hpp"
#include "core/log.hpp"
#include "core/steam_login/session.hpp"
#include "core/time_aligner.hpp"

namespace sam::sda {

using json = nlohmann::json;

namespace {

constexpr char kMobileClient[] = "android";
constexpr char kMobileClientVersion[] = "777777 3.6.4";

constexpr char kUserAgent[] =
    "Dalvik/2.1.0 (Linux; U; Android 9; Valve Steam App Version/3)";

std::string steam_login_secure_value(const core::Account& a) {
    return steam_login::make_steam_login_secure(a.steam_id_64, a.access_token);
}

std::string jwt_from_cookie(const std::string& cookie_value) {
    const auto sep = cookie_value.find("%7C%7C");
    if (sep == std::string::npos) return {};
    return cookie_value.substr(sep + 6);
}

ConfirmationType detect_type(int t) {

    switch (t) {
        case 2: return ConfirmationType::Trade;
        case 3: return ConfirmationType::MarketListing;
        case 6: return ConfirmationType::AccountRecovery;
        case 7: return ConfirmationType::PhoneNumberChange;
        case 8: return ConfirmationType::FeatureOptOut;
        default: return ConfirmationType::Unknown;
    }
}

std::string make_confirmation_hash(const std::string& identity_secret_b64,
                                    std::int64_t time,
                                    std::string_view tag) {
    const auto key = crypto::base64_decode(identity_secret_b64);
    if (key.empty()) return {};

    std::vector<std::uint8_t> buf;
    buf.reserve(8 + tag.size());
    {
        std::int64_t t = time;
        std::array<std::uint8_t, 8> be{};
        for (int i = 7; i >= 0; --i) {
            be[i] = static_cast<std::uint8_t>(t & 0xFF);
            t >>= 8;
        }
        buf.insert(buf.end(), be.begin(), be.end());
    }
    buf.insert(buf.end(), tag.begin(), tag.end());

    const auto mac = crypto::hmac_sha1(
        std::span<const std::uint8_t>{key.data(), key.size()},
        std::span<const std::uint8_t>{buf.data(), buf.size()});

    return crypto::base64_encode(std::span<const std::uint8_t>{mac.data(), mac.size()});
}

std::string base_query(const core::Account& a, std::string_view tag) {
    if (!a.sda.has_value()) return {};
    const auto& sda = *a.sda;

    const std::int64_t t = time_aligner::aligned_now();
    const std::string k = make_confirmation_hash(sda.identity_secret, t, tag);
    if (k.empty()) return {};

    std::string out;
    out += "p=" + sda.device_id;
    out += "&a=" + std::to_string(a.steam_id_64);
    out += "&k=" + http::url_encode(k);
    out += "&t=" + std::to_string(t);
    out += "&m=react";
    out += "&tag=" + std::string(tag);
    return out;
}

void apply_cookies(http::Request& req, const core::Account& a) {

    req.cookies.push_back({"steamLoginSecure",
                            steam_login_secure_value(a),
                            "steamcommunity.com", "/", true, true});
    req.cookies.push_back({"sessionid", a.session_id,
                            "steamcommunity.com", "/", true, true});
    req.cookies.push_back({"mobileClient", kMobileClient,
                            "steamcommunity.com", "/", true, true});
    req.cookies.push_back({"mobileClientVersion", kMobileClientVersion,
                            "steamcommunity.com", "/", true, true});
}

http::Request build_request(http::Method method,
                             const std::string& url,
                             const core::Account& a) {
    http::Request req;
    req.method = method;
    req.url = url;
    req.user_agent = kUserAgent;

    apply_cookies(req, a);
    return req;
}

}  // namespace

ConfirmationFetchResult fetch_confirmations(const core::Account& a) {
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    ConfirmationFetchResult out;

    if (!a.sda.has_value() || a.session_id.empty() || a.steam_id_64 == 0 ||
        a.access_token.empty()) {
        out.error = "account has no active session";
        return out;
    }

    if (a.sda->identity_secret.empty() || a.sda->device_id.empty()) {
        out.error = "missing maFile data (identity_secret/device_id)";
        return out;
    }

    const std::string query = base_query(a, "conf");
    if (query.empty()) {
        out.error = "missing identity_secret";
        return out;
    }

    const std::string url =
        "https://steamcommunity.com/mobileconf/getlist?" + query;

    auto resp = http::request(build_request(http::Method::Get, url, a));
    out.http_status = static_cast<int>(resp.status);
    if (resp.status != 200) {
        out.error = "http " + std::to_string(resp.status);
        return out;
    }

    auto string_field = [](const json& j, const char* key) -> std::string {
        if (!j.contains(key)) return {};
        const auto& v = j[key];
        if (v.is_string())          return v.get<std::string>();
        if (v.is_number_unsigned()) return std::to_string(v.get<std::uint64_t>());
        if (v.is_number_integer())  return std::to_string(v.get<std::int64_t>());
        return {};
    };

    try {
        const auto j = json::parse(resp.body);
        if (!j.value("success", false)) {
            out.error = j.value("message", std::string{"steam returned success=false"});
            return out;
        }
        if (j.value("needauth", false)) {
            out.error = "session expired, re-import maFile or run the login wizard";
            return out;
        }
        if (j.contains("conf") && j["conf"].is_array()) {
            for (const auto& c : j["conf"]) {
                Confirmation item;
                item.id            = string_field(c, "id");
                item.nonce         = string_field(c, "nonce");
                item.creator_id    = string_field(c, "creator_id");
                item.headline      = c.value("headline", "");
                if (c.contains("summary") && c["summary"].is_array() && !c["summary"].empty()) {
                    item.summary = c["summary"][0].get<std::string>();
                } else {
                    item.summary = c.value("type_name", "");
                }
                item.icon_url      = c.value("icon", "");
                item.creation_unix = c.value("creation_time", 0);
                item.type          = detect_type(c.value("type", 0));
                out.items.push_back(std::move(item));
            }
        }
        out.ok = true;
    } catch (const std::exception& ex) {
        out.error = std::string{"parse: "} + ex.what();
    }

    return out;
}

bool respond_to_confirmation(const core::Account& a,
                              const Confirmation& c,
                              bool allow,
                              std::string* error_out) {
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    if (!a.sda.has_value() ||
        a.sda->identity_secret.empty() || a.sda->device_id.empty()) {
        if (error_out) *error_out = "missing maFile data (identity_secret/device_id)";
        return false;
    }
    const std::string_view op  = allow ? "allow"  : "cancel";
    const std::string_view tag = allow ? "accept" : "reject";
    const std::string query = base_query(a, tag);
    if (query.empty()) {
        if (error_out) *error_out = "missing identity_secret";
        return false;
    }
    const std::string url =
        "https://steamcommunity.com/mobileconf/ajaxop?op=" + std::string(op) +
        "&" + query + "&cid=" + c.id + "&ck=" + c.nonce;

    auto resp = http::request(build_request(http::Method::Get, url, a));
    if (resp.status != 200) {
        if (error_out) *error_out = "http " + std::to_string(resp.status);
        return false;
    }

    try {
        const auto j = json::parse(resp.body);
        const bool success = j.value("success", false);
        if (!success) {

            std::string set_cookies;
            for (const auto& sc : resp.set_cookies) {
                if (!set_cookies.empty()) set_cookies += " | ";
                set_cookies += sc;
            }
            const std::string cookie_val = steam_login_secure_value(a);
            const std::string cookie_jwt = jwt_from_cookie(cookie_val);
            const std::string cookie_aud = steam_login::jwt_audience(
                crypto::make_secure(cookie_jwt));
            const std::string sess_tail =
                a.session_id.size() >= 4
                    ? a.session_id.substr(a.session_id.size() - 4) : "";
            SAM_LOG_WARN("confirmation respond: '{}' url='{}' status={} "
                         "body='{}' set_cookie='{}' time_offset={}s "
                         "cookie_aud='{}' sess='{}..{}' aud_acc='{}'",
                         a.login, url, resp.status, resp.body, set_cookies,
                         time_aligner::offset_seconds(),
                         cookie_aud,
                         a.session_id.substr(0, 4), sess_tail,
                         steam_login::jwt_audience(a.access_token));
            if (error_out) {
                const std::string msg = j.value("message", std::string{});
                *error_out = msg.empty() ? std::string{kRespondSuccessFalse} : msg;
            }
        }
        return success;
    } catch (const std::exception& ex) {
        if (error_out) *error_out = ex.what();
        return false;
    }
}

namespace {
constexpr std::size_t kBulkChunkCap = 50;

bool respond_bulk_chunk(const core::Account& a,
                         const Confirmation* items, std::size_t n,
                         bool allow, std::string* error_out);
}

bool respond_bulk(const core::Account& a,
                   const std::vector<Confirmation>& items,
                   bool allow,
                   std::string* error_out) {
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    if (items.empty()) return true;

    if (items.size() <= kBulkChunkCap) {
        return respond_bulk_chunk(a, items.data(), items.size(), allow, error_out);
    }

    for (std::size_t off = 0; off < items.size(); off += kBulkChunkCap) {
        const std::size_t n = std::min(kBulkChunkCap, items.size() - off);
        std::string err;
        if (!respond_bulk_chunk(a, items.data() + off, n, allow, &err)) {
            if (error_out) *error_out = std::move(err);
            return false;
        }
    }
    return true;
}

namespace {
bool respond_bulk_chunk(const core::Account& a,
                         const Confirmation* items, std::size_t n,
                         bool allow, std::string* error_out) {
    if (n == 0) return true;
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    if (!a.sda.has_value() ||
        a.sda->identity_secret.empty() || a.sda->device_id.empty()) {
        if (error_out) *error_out = "missing maFile data (identity_secret/device_id)";
        return false;
    }
    const std::string_view op  = allow ? "allow"  : "cancel";
    const std::string_view tag = allow ? "accept" : "reject";
    const std::string query = base_query(a, tag);
    if (query.empty()) {
        if (error_out) *error_out = "missing identity_secret";
        return false;
    }

    std::string body = "op=" + std::string(op) + "&" + query;
    for (std::size_t i = 0; i < n; ++i) {
        body += "&cid[]=" + items[i].id;
        body += "&ck[]=" + items[i].nonce;
    }

    http::Request req = build_request(http::Method::Post,
                                       "https://steamcommunity.com/mobileconf/multiajaxop", a);
    req.headers["Content-Type"] = "application/x-www-form-urlencoded; charset=UTF-8";
    req.body = std::move(body);

    auto resp = http::request(req);
    if (resp.status != 200) {
        if (error_out) *error_out = "http " + std::to_string(resp.status);
        return false;
    }

    try {
        const auto j = json::parse(resp.body);
        const bool success = j.value("success", false);
        if (!success) {
            std::string set_cookies;
            for (const auto& sc : resp.set_cookies) {
                if (!set_cookies.empty()) set_cookies += " | ";
                set_cookies += sc;
            }
            const std::string cookie_val = steam_login_secure_value(a);
            const std::string cookie_jwt = jwt_from_cookie(cookie_val);
            const std::string cookie_aud = steam_login::jwt_audience(
                crypto::make_secure(cookie_jwt));
            const std::string sess_tail =
                a.session_id.size() >= 4
                    ? a.session_id.substr(a.session_id.size() - 4) : "";
            SAM_LOG_WARN("confirmation respond_bulk: '{}' status={} "
                         "body='{}' set_cookie='{}' time_offset={}s "
                         "cookie_aud='{}' sess='{}..{}' aud_acc='{}'",
                         a.login, resp.status, resp.body, set_cookies,
                         time_aligner::offset_seconds(),
                         cookie_aud,
                         a.session_id.substr(0, 4), sess_tail,
                         steam_login::jwt_audience(a.access_token));
            if (error_out) {
                const std::string msg = j.value("message", std::string{});
                *error_out = msg.empty() ? std::string{kRespondSuccessFalse} : msg;
            }
        }
        return success;
    } catch (const std::exception& ex) {
        if (error_out) *error_out = ex.what();
        return false;
    }
}
}  // namespace

}  // namespace sam::sda
