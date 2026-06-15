#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::steam_login {

// steamLoginSecure cookie value in the form Steam expects: "<steamid>%7C%7C<jwt>".
std::string make_steam_login_secure(std::uint64_t steam_id_64,
                                     const crypto::SecureString& access_token);

// `exp` claim from a JWT, 0 if unparseable.
std::int64_t jwt_expiry(const crypto::SecureString& jwt);

// `aud` claim joined with commas, e.g. "mobile,web". Only tokens with "mobile"
// in the audience are accepted by /mobileconf/ajaxop.
std::string jwt_audience(const crypto::SecureString& jwt);

// `iss` claim, empty if unparseable. Genuine Steam tokens carry iss == "steam".
std::string jwt_issuer(const crypto::SecureString& jwt);

// SteamID64 from the `sub` claim (decimal string), 0 if unparseable.
std::uint64_t jwt_steam_id(const crypto::SecureString& jwt);

// True if the access token is missing or `exp` is within `safety_seconds` of now.
bool needs_refresh(const core::Account& account, int safety_seconds = 300);

// Refreshes the access token from the stored refresh token. Caller saves the vault.
bool refresh_access_token(core::Account& account);

// One per-domain token-transfer endpoint from `jwt/finalizelogin`. `auth`/`nonce`
// are single-use and short-lived.
struct TransferTarget {
    std::string url;
    std::string nonce;
    std::string auth;
};

// Runs only the `jwt/finalizelogin` step, returning every per-domain transfer
// target. `redir` is where Steam sends the browser after the transfer. The
// `nonce` posted is the refresh_token, not the access_token (latter returns
// "Access is denied").
bool finalize_login_targets(std::uint64_t steam_id_64,
                            const crypto::SecureString& refresh_token,
                            const std::string& session_id,
                            const std::string& redir,
                            std::vector<TransferTarget>& out_targets);

// Exchanges a refresh_token for a community-valid steamLoginSecure cookie via
// `jwt/finalizelogin` + `settoken`. The finalizelogin `nonce` is the
// refresh_token; the access_token returns "Access is denied (15)".
bool transfer_login(std::uint64_t steam_id_64,
                    const crypto::SecureString& refresh_token,
                    const std::string& session_id,
                    std::string& out_cookie);

}  // namespace sam::steam_login
