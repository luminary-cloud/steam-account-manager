#include "core/profile/edit.hpp"

#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "core/crypto/rng.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/http/client.hpp"
#include "core/log.hpp"
#include "core/steam_login/session.hpp"

namespace sam::profile {

using json = nlohmann::json;

namespace {

std::string steam_login_secure_cookie(const core::Account& a) {
    // Prefer the settoken web cookie (exactly what the browser sends for
    // steamcommunity.com); fall back to the access-token-derived value.
    if (!a.steam_login_secure.empty()) {
        return std::string(a.steam_login_secure.begin(), a.steam_login_secure.end());
    }
    return steam_login::make_steam_login_secure(a.steam_id_64, a.access_token);
}

// Ensures `a` has a usable web session: a non-expired access token and a
// community steamLoginSecure cookie. Mirrors the session maintenance the
// refresh worker performs in app_state.cpp. Returns false when no session can
// be established from the stored tokens (caller should fall back to a full
// credentials re-login).
bool ensure_web_session(core::Account& a) {
    if (a.session_id.empty()) {
        a.session_id = crypto::random_session_id();
    }
    if (steam_login::needs_refresh(a, 300)) {
        // Best-effort: the cookie mint below is the real gate.
        steam_login::refresh_access_token(a);
    }
    if (!a.refresh_token.empty()) {
        std::string cookie;
        if (steam_login::transfer_login(a.steam_id_64, a.refresh_token,
                                        a.session_id, cookie)) {
            a.steam_login_secure = crypto::make_secure(cookie);
        }
    }
    return a.steam_id_64 != 0 && !a.session_id.empty() &&
           !steam_login_secure_cookie(a).empty();
}

}  // namespace

PersonaChangeResult change_persona_name(core::Account& a, const std::string& new_name) {
    PersonaChangeResult out;

    if (a.steam_id_64 == 0) {
        out.error = "account has no resolved SteamID";
        return out;
    }
    if (new_name.empty()) {
        out.error = "name is empty";
        return out;
    }

    if (!ensure_web_session(a)) {
        out.error = "session expired - refresh or re-login this account";
        out.needs_relogin = true;
        return out;
    }

    http::Request req;
    req.method = http::Method::Post;
    req.url = "https://steamcommunity.com/profiles/" +
              std::to_string(a.steam_id_64) + "/ajaxsetpersonaname/";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded; charset=UTF-8";
    req.body = http::form_encode({
        {"sessionid", a.session_id},
        {"persona", new_name},
    });
    req.cookies.push_back({"steamLoginSecure", steam_login_secure_cookie(a),
                            "steamcommunity.com", "/", true, true});
    req.cookies.push_back({"sessionid", a.session_id,
                            "steamcommunity.com", "/", true, true});

    auto resp = http::request(req);
    if (resp.status == 401 || resp.status == 403) {
        out.error = "http " + std::to_string(resp.status);
        out.needs_relogin = true;
        return out;
    }
    if (resp.status != 200) {
        out.error = "http " + std::to_string(resp.status);
        SAM_LOG_WARN("change_persona_name: http {} body='{}'", resp.status, resp.body);
        return out;
    }

    try {
        const auto j = json::parse(resp.body);
        // ajaxsetpersonaname echoes the applied name back in "success" and only
        // populates "message" when it rejects the change, e.g.
        //   {"success":"NewName","message":""}            -> applied
        //   {"success":"","message":"That name is taken"} -> rejected
        // So an empty (or absent) "message" means the rename went through.
        std::string message;
        if (j.contains("message") && j.at("message").is_string()) {
            message = j.at("message").get<std::string>();
        }
        if (!message.empty()) {
            out.error = message;
            SAM_LOG_WARN("change_persona_name: rejected, body='{}'", resp.body);
            return out;
        }
        out.ok = true;
        if (j.contains("success") && j.at("success").is_string()) {
            out.applied_name = j.at("success").get<std::string>();
        }
        return out;
    } catch (const std::exception& ex) {
        out.error = "unexpected response from Steam";
        SAM_LOG_WARN("change_persona_name: parse error {} body='{}'", ex.what(), resp.body);
    }
    return out;
}

}  // namespace sam::profile
