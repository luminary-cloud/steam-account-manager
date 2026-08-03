#pragma once

#include <string>

#include "core/account_store/account.hpp"

namespace sam::steam_login {

// Ensures `a` has a usable community web session, mutating `a` with any
// refreshed tokens/cookie for the caller to persist. False when no session can
// be established (a dead refresh token); surface a re-login.
// Must run on a worker thread: it makes blocking network requests.
bool ensure_web_session(core::Account& a);

// Mints a community session over a CM logon and writes the access token, its expiry, and the
// steamLoginSecure cookie into `a`. This is the only route that works for a client-scoped
// (NFA/cached) token, which the HTTP auth endpoints refuse. Opens a CM connection, so callers
// outside ensure_web_session must throttle it: Steam rate-limits repeated logons.
bool mint_web_session_via_cm(core::Account& a);

// steamLoginSecure cookie for steamcommunity.com. This is the WEB cookie;
// mobile-confirmation code builds its own mobile-audience cookie separately.
std::string web_login_secure_cookie(const core::Account& a);

}  // namespace sam::steam_login
