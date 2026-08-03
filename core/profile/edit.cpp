#include "core/profile/edit.hpp"

#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "core/http/client.hpp"
#include "core/log.hpp"
#include "core/steam_login/web_session.hpp"

namespace sam::profile {

using json = nlohmann::json;

PersonaChangeResult change_persona_name(core::Account& a, const std::string& new_name) {
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    PersonaChangeResult out;

    if (a.steam_id_64 == 0) {
        out.error = "account has no resolved SteamID";
        return out;
    }
    if (new_name.empty()) {
        out.error = "name is empty";
        return out;
    }

    if (!steam_login::ensure_web_session(a)) {
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
    req.cookies.push_back({"steamLoginSecure", steam_login::web_login_secure_cookie(a),
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
