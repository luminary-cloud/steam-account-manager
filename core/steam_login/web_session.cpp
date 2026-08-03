#include "core/steam_login/web_session.hpp"

#include "core/crypto/rng.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/log.hpp"
#include "core/steam_cm/web_token.hpp"
#include "core/steam_login/session.hpp"

namespace sam::steam_login {

std::string web_login_secure_cookie(const core::Account& a) {
    if (!a.steam_login_secure.empty()) {
        return std::string(a.steam_login_secure.begin(), a.steam_login_secure.end());
    }
    return make_steam_login_secure(a.steam_id_64, a.access_token);
}

bool ensure_web_session(core::Account& a) {
    if (a.session_id.empty()) {
        a.session_id = crypto::random_session_id();
    }

    if (a.is_nfa) {

        if (web_login_secure_cookie(a).empty() || needs_refresh(a, 300)) {
            mint_web_session_via_cm(a);
        }
    } else {
        if (needs_refresh(a, 300)) {
            refresh_access_token(a);
        }
        if (!a.refresh_token.empty()) {
            std::string cookie;
            if (transfer_login(a.steam_id_64, a.refresh_token, a.session_id, cookie)) {
                a.steam_login_secure = crypto::make_secure(cookie);
            }
        }
    }
    return a.steam_id_64 != 0 && !a.session_id.empty() &&
           !web_login_secure_cookie(a).empty();
}

bool mint_web_session_via_cm(core::Account& a) {
    if (a.refresh_token.empty() || a.steam_id_64 == 0) return false;

    std::string error;
    auto minted = steam_cm::mint_web_access_token(a.refresh_token, a.steam_id_64, error);
    if (!minted) {
        SAM_LOG_WARN("web-session: CM mint failed for '{}': {}", a.login, error);
        return false;
    }

    if (!minted->rotated_refresh_token.empty()) {
        a.refresh_token = minted->rotated_refresh_token;
        a.refresh_token_expires = jwt_expiry(a.refresh_token);
    }
    if (a.session_id.empty()) a.session_id = crypto::random_session_id();
    a.access_token = minted->access_token;
    a.access_token_expires = jwt_expiry(a.access_token);
    a.steam_login_secure =
        crypto::make_secure(make_steam_login_secure(a.steam_id_64, a.access_token));
    SAM_LOG_INFO("web-session: minted a community session for '{}' over the CM (token exp={})",
                 a.login, a.access_token_expires);
    return true;
}

}  // namespace sam::steam_login
