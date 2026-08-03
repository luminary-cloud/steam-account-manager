#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/crypto/secure_string.hpp"

namespace sam::steam_cm {

struct MintedAccessToken {
    // Web-audience JWT. Scoped to the community domain: it is one access token, so it covers
    // steamcommunity.com and nothing else. The help/store domains need their own per-domain
    // bootstrap, which only the cold-HTTP finalizelogin/refresh endpoints can do.
    crypto::SecureString access_token;
    // Normally empty. Set only if Steam rotated the credential anyway, in which case the
    // caller MUST persist it: the token passed in is dead from that point on.
    crypto::SecureString rotated_refresh_token;
};

// Mints a web access token for `refresh_token` by logging on to a CM and asking that
// authenticated channel for one.
//
// This exists because the cold-HTTP route (GenerateAccessTokenForApp with the token as a
// Bearer header) answers AccessDenied for a client-scoped token, the kind NFA and cached
// accounts hold. The same RPC succeeds as a unified message over a logged-on CM session.
//
// Non-destructive: token renewal is never requested, so the refresh token is neither rotated
// nor consumed. Blocks for up to a few seconds and opens a CM connection, so call it from a
// worker thread, never the UI thread. Steam rate-limits rapid CM logons, so callers must
// throttle; it also honours whatever proxy is installed on the calling thread.
//
// Returns nullopt with `error` filled on any failure.
std::optional<MintedAccessToken> mint_web_access_token(const crypto::SecureString& refresh_token,
                                                       std::uint64_t steam_id,
                                                       std::string& error);

}  // namespace sam::steam_cm
