#pragma once

#include <string>

#include "core/account_store/account.hpp"

namespace sam::steam_login {

// Ensures `a` has a usable community web session, mutating `a` with any
// refreshed tokens/cookie for the caller to persist. False when no session can
// be established (NFA accounts or a dead refresh token); surface a re-login.
// Must run on a worker thread: it makes blocking network requests.
bool ensure_web_session(core::Account& a);

// steamLoginSecure cookie for steamcommunity.com. This is the WEB cookie;
// mobile-confirmation code builds its own mobile-audience cookie separately.
std::string web_login_secure_cookie(const core::Account& a);

}  // namespace sam::steam_login
