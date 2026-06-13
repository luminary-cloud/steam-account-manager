#include "core/steam_login/web_session.hpp"

#include "core/crypto/rng.hpp"
#include "core/crypto/secure_string.hpp"
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
    if (needs_refresh(a, 300)) {
        refresh_access_token(a);  // best-effort; the cookie mint below is the gate
    }
    if (!a.refresh_token.empty()) {
        std::string cookie;
        if (transfer_login(a.steam_id_64, a.refresh_token, a.session_id, cookie)) {
            a.steam_login_secure = crypto::make_secure(cookie);
        }
    }
    return a.steam_id_64 != 0 && !a.session_id.empty() &&
           !web_login_secure_cookie(a).empty();
}

}  // namespace sam::steam_login
