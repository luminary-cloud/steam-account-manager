#include "core/steam_login/session.hpp"

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/crypto/base64.hpp"
#include "core/http/client.hpp"
#include "core/http/url.hpp"
#include "core/log.hpp"

#include "core/steam_auth/gen/steammessages_auth.steamclient.pb.h"

namespace sam::steam_login {

using json = nlohmann::json;

namespace {

std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string jwt_payload(std::string_view jwt) {
    const auto first = jwt.find('.');
    if (first == std::string_view::npos) return {};
    const auto second = jwt.find('.', first + 1);
    if (second == std::string_view::npos) return {};
    std::string seg(jwt.substr(first + 1, second - first - 1));

    while (seg.size() % 4 != 0) seg += '=';
    const auto raw = crypto::base64_decode(seg);
    return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
}

}  // namespace

std::string make_steam_login_secure(std::uint64_t steam_id_64,
                                     const crypto::SecureString& access_token) {
    if (steam_id_64 == 0 || access_token.empty()) return {};
    std::string token(access_token.begin(), access_token.end());

    return std::to_string(steam_id_64) + "%7C%7C" + token;
}

std::int64_t jwt_expiry(const crypto::SecureString& jwt) {
    if (jwt.empty()) return 0;
    const std::string payload = jwt_payload(std::string_view{jwt.data(), jwt.size()});
    if (payload.empty()) return 0;
    try {
        const auto j = json::parse(payload);
        if (j.contains("exp")) return j["exp"].get<std::int64_t>();
    } catch (...) {
        return 0;
    }
    return 0;
}

std::uint64_t jwt_steam_id(const crypto::SecureString& jwt) {
    if (jwt.empty()) return 0;
    const std::string payload = jwt_payload(std::string_view{jwt.data(), jwt.size()});
    if (payload.empty()) return 0;
    try {
        const auto j = json::parse(payload);
        if (!j.contains("sub")) return 0;
        const auto& sub = j["sub"];

        if (sub.is_string()) {
            const auto& s = sub.get_ref<const std::string&>();
            if (s.empty()) return 0;
            std::uint64_t out = 0;
            try { out = std::stoull(s); } catch (...) { return 0; }
            return out;
        }
        if (sub.is_number_unsigned()) return sub.get<std::uint64_t>();
    } catch (...) {
        return 0;
    }
    return 0;
}

std::string jwt_audience(const crypto::SecureString& jwt) {
    if (jwt.empty()) return {};
    const std::string payload = jwt_payload(std::string_view{jwt.data(), jwt.size()});
    if (payload.empty()) return {};
    try {
        const auto j = json::parse(payload);
        if (!j.contains("aud")) return {};
        const auto& aud = j["aud"];
        if (aud.is_string()) return aud.get<std::string>();
        if (!aud.is_array()) return {};
        std::string out;
        for (const auto& v : aud) {
            if (!v.is_string()) continue;
            if (!out.empty()) out += ",";
            out += v.get<std::string>();
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::string jwt_issuer(const crypto::SecureString& jwt) {
    if (jwt.empty()) return {};
    const std::string payload = jwt_payload(std::string_view{jwt.data(), jwt.size()});
    if (payload.empty()) return {};
    try {
        const auto j = json::parse(payload);
        if (!j.contains("iss")) return {};
        const auto& iss = j["iss"];
        if (iss.is_string()) return iss.get<std::string>();
    } catch (...) {
        return {};
    }
    return {};
}

bool needs_refresh(const core::Account& a, int safety_seconds) {
    if (a.access_token.empty()) return true;
    const std::int64_t exp = a.access_token_expires == 0 ? jwt_expiry(a.access_token)
                                                          : a.access_token_expires;
    if (exp == 0) return true;
    return (now_seconds() + safety_seconds) >= exp;
}

namespace {

std::optional<std::string> extract_cookie(const std::string& set_cookie_value,
                                          std::string_view name) {
    if (set_cookie_value.size() < name.size() + 1) return std::nullopt;
    if (set_cookie_value.compare(0, name.size(), name) != 0) return std::nullopt;
    if (set_cookie_value[name.size()] != '=') return std::nullopt;
    const auto value_start = name.size() + 1;
    const auto semi = set_cookie_value.find(';', value_start);
    if (semi == std::string::npos) return set_cookie_value.substr(value_start);
    return set_cookie_value.substr(value_start, semi - value_start);
}

}  // namespace

bool finalize_login_targets(std::uint64_t steam_id_64,
                            const crypto::SecureString& refresh_token,
                            const std::string& session_id,
                            const std::string& redir,
                            std::vector<TransferTarget>& out_targets) {
    out_targets.clear();
    if (steam_id_64 == 0 || refresh_token.empty() || session_id.empty()) {
        SAM_LOG_WARN("finalize_login_targets: missing inputs (sid={} rt_set={} session_set={})",
                     steam_id_64, !refresh_token.empty(), !session_id.empty());
        return false;
    }

    const std::string rt(refresh_token.begin(), refresh_token.end());

    http::Request fin;
    fin.method = http::Method::Post;
    fin.url = "https://login.steampowered.com/jwt/finalizelogin";
    fin.headers["Content-Type"] = "application/x-www-form-urlencoded";
    fin.headers["Origin"]  = "https://steamcommunity.com";
    fin.headers["Referer"] = "https://steamcommunity.com/";
    fin.cookies.push_back({"sessionid", session_id,
                            "login.steampowered.com", "/", true, false});
    fin.body = "nonce=" + http::url_encode(rt) +
               "&sessionid=" + http::url_encode(session_id) +
               "&redir=" + http::url_encode(redir);

    SAM_LOG_INFO("finalize_login_targets: POST finalizelogin");
    const auto fin_resp = http::request(fin);
    if (fin_resp.status != 200) {
        SAM_LOG_WARN("finalize_login_targets: finalizelogin status {} body='{}'",
                     fin_resp.status, fin_resp.body);
        return false;
    }

    json j;
    try {
        j = json::parse(fin_resp.body);
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("finalize_login_targets: finalizelogin parse failed: {}", ex.what());
        return false;
    }

    if (!j.contains("transfer_info") || !j["transfer_info"].is_array()) {
        SAM_LOG_WARN("finalize_login_targets: no transfer_info in response, body='{}'",
                     fin_resp.body);
        return false;
    }

    for (const auto& t : j["transfer_info"]) {
        const std::string url = t.value("url", "");
        if (url.empty()) continue;
        if (!t.contains("params") || !t["params"].is_object()) continue;
        const auto& params = t["params"];
        const std::string nonce = params.value("nonce", "");
        const std::string auth  = params.value("auth", "");
        if (nonce.empty() || auth.empty()) continue;
        out_targets.push_back({url, nonce, auth});
    }

    SAM_LOG_INFO("finalize_login_targets: {} target(s)", out_targets.size());
    return !out_targets.empty();
}

bool transfer_login(std::uint64_t steam_id_64,
                    const crypto::SecureString& refresh_token,
                    const std::string& session_id,
                    std::string& out_cookie) {
    std::vector<TransferTarget> targets;
    if (!finalize_login_targets(steam_id_64, refresh_token, session_id,
                                "https://steamcommunity.com/login/home/?goto=", targets)) {
        return false;
    }

    for (const auto& t : targets) {
        if (t.url.find("steamcommunity.com") == std::string::npos) continue;

        http::Request st;
        st.method = http::Method::Post;
        st.url = t.url;
        st.headers["Content-Type"] = "application/x-www-form-urlencoded";
        st.follow_redirects = false;
        st.body = "nonce=" + http::url_encode(t.nonce) +
                  "&auth="  + http::url_encode(t.auth)  +
                  "&steamID=" + std::to_string(steam_id_64);

        SAM_LOG_INFO("transfer_login: POST {}", t.url);
        const auto st_resp = http::request(st);
        SAM_LOG_INFO("transfer_login: settoken status={} cookies={}",
                     st_resp.status, st_resp.set_cookies.size());

        for (const auto& sc : st_resp.set_cookies) {
            if (auto v = extract_cookie(sc, "steamLoginSecure")) {
                if (!v->empty()) {
                    out_cookie = *v;
                    SAM_LOG_INFO("transfer_login: got steamLoginSecure ({} chars)",
                                 out_cookie.size());
                    return true;
                }
            }
        }
    }

    SAM_LOG_WARN("transfer_login: no steamLoginSecure cookie found in settoken responses");
    return false;
}

bool refresh_access_token(core::Account& a) {
    if (a.refresh_token.empty()) return false;

    CAuthentication_AccessToken_GenerateForApp_Request body;
    const std::string rt(a.refresh_token.begin(), a.refresh_token.end());
    body.set_refresh_token(rt);
    body.set_steamid(a.steam_id_64);

    std::string serialized;
    if (!body.SerializeToString(&serialized)) {
        SAM_LOG_WARN("session: refresh: failed to serialize request proto");
        return false;
    }
    const std::string b64 = crypto::base64_encode(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size()));

    http::Request req;
    req.method = http::Method::Post;
    req.url = "https://api.steampowered.com/IAuthenticationService/"
              "GenerateAccessTokenForApp/v1/";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["Authorization"] = "Bearer " + rt;
    req.headers["Accept"] = "application/x-protobuf";
    req.body = "input_protobuf_encoded=" + http::url_encode(b64);

    SAM_LOG_INFO("session: refresh: POST GenerateAccessTokenForApp (proto, {} bytes, Bearer header)",
                 serialized.size());
    auto resp = http::request(req);
    if (resp.status != 200) {
        SAM_LOG_WARN("session: refresh http {} body='{}'", resp.status, resp.body);
        return false;
    }

    auto find_header = [&](const std::string& name) -> std::string {
        const auto it = resp.headers.find(name);
        return it != resp.headers.end() ? it->second : std::string{};
    };
    const std::string er  = find_header("x-eresult");
    const std::string em  = find_header("x-error_message");
    const std::string raw_b64 = crypto::base64_encode(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(resp.body.data()),
            resp.body.size()));
    SAM_LOG_INFO("session: refresh: response status={} body_len={} x-eresult='{}' x-error_message='{}' body_b64='{}'",
                 resp.status, resp.body.size(), er, em, raw_b64);

    CAuthentication_AccessToken_GenerateForApp_Response parsed;
    if (!parsed.ParseFromArray(resp.body.data(), static_cast<int>(resp.body.size()))) {
        SAM_LOG_WARN("session: refresh: failed to parse response proto ({} bytes)",
                     resp.body.size());
        return false;
    }

    const std::string at = parsed.access_token();
    if (at.empty()) {

        SAM_LOG_WARN("session: refresh: empty access_token in response (x-eresult={} x-error_message='{}')",
                     er.empty() ? "?" : er, em);
        return false;
    }

    a.access_token = crypto::make_secure(at);
    a.access_token_expires = jwt_expiry(a.access_token);
    a.steam_login_secure = crypto::make_secure(
        make_steam_login_secure(a.steam_id_64, a.access_token));
    if (!parsed.refresh_token().empty()) {

        a.refresh_token = crypto::make_secure(parsed.refresh_token());
    }
    SAM_LOG_INFO("session: refresh ok, new access_token exp={} ({} chars)",
                 a.access_token_expires, at.size());
    return true;
}

}  // namespace sam::steam_login
