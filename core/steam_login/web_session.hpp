#pragma once

#include <string>

#include "core/account_store/account.hpp"

namespace sam::steam_login {

// Ensures `a` has a usable community web session: a non-expired access token, a
// sessionid, and a steamLoginSecure cookie minted via transfer_login. Mutates
// `a` with any refreshed tokens/cookie so the caller can persist them. Returns
// false when no session can be established (NFA accounts, or a dead refresh
// token); the caller should surface a re-login prompt.
//
// Must run on a worker thread: it performs blocking network requests.
bool ensure_web_session(core::Account& a);

// The steamLoginSecure cookie value to send to steamcommunity.com. Prefers the
// settoken/community cookie (`a.steam_login_secure`) and falls back to the
// access-token-derived value. This is the WEB cookie; mobile-confirmation code
// builds its own mobile-audience cookie separately.
std::string web_login_secure_cookie(const core::Account& a);

}  // namespace sam::steam_login
