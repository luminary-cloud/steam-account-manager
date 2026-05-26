#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::steam_login {

// Constructs the steamLoginSecure cookie value from `steam_id_64` and the JWT
// access token, in the form Steam expects: "<steamid>%7C%7C<jwt>".
std::string make_steam_login_secure(std::uint64_t steam_id_64,
                                     const crypto::SecureString& access_token);

// Decodes the `exp` claim from a JWT. Returns 0 if it can't be parsed.
std::int64_t jwt_expiry(const crypto::SecureString& jwt);

// Returns the `aud` claim joined with commas, e.g. "mobile,web". Empty if the
// token can't be parsed. Only tokens with "mobile" in the audience are accepted
// by /mobileconf/ajaxop.
std::string jwt_audience(const crypto::SecureString& jwt);

// Returns the SteamID64 from the `sub` claim. 0 if the token can't be parsed
// or the claim isn't a numeric string. Lets callers recover the steam_id when
// the source file and loginusers.vdf both lack one.
std::uint64_t jwt_steam_id(const crypto::SecureString& jwt);

// Returns true if the access token is missing or its `exp` is within
// `safety_seconds` of now.
bool needs_refresh(const core::Account& account, int safety_seconds = 300);

// Refreshes the access token using the stored refresh token. Returns true on
// success. The caller is responsible for saving the vault afterwards.
bool refresh_access_token(core::Account& account);

// Exchanges a refresh_token for a community-valid steamLoginSecure cookie via
// Steam's `jwt/finalizelogin` + `settoken` flow.
//
// The `nonce` field on finalizelogin is the refresh_token, not the
// access_token. Sending the access_token returns "Access is denied (15)". Use
// the refresh_token minted by a BeginAuthSessionViaCredentials login with
// website_id="Community".
bool transfer_login(std::uint64_t steam_id_64,
                    const crypto::SecureString& refresh_token,
                    const std::string& session_id,
                    std::string& out_cookie);

}  // namespace sam::steam_login
