#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sam::steam_gcpd {

// Fields extracted from /gcpd/730?tab=matchmaking: covers everything the
// account-manager UI consumes for CS2: premier rating, wingman rank, and
// the active competitive cooldown (if any).
struct MatchmakingData {
    bool ok = false;
    int  premier_rating = -1;
    int  premier_wins   = -1;
    int  wingman_rank   = -1;
    int  wingman_wins   = -1;
    std::int64_t cooldown_expires_unix = 0;   // 0 = no active cooldown
    std::string  cooldown_reason;
};

// Fields extracted from /gcpd/730?tab=accountmain: CS2 in-game level + XP,
// distinct from the Steam profile level which comes from the Web API.
struct AccountMainData {
    bool ok = false;
    int  cs2_player_level = -1;
    int  cs2_player_xp    = -1;
};

MatchmakingData parse_matchmaking(std::string_view html);
AccountMainData parse_accountmain(std::string_view html);

// Quick content-signature checks used to distinguish a real GCPD response
// from the login page Steam serves when the session cookie is invalid.
// Both check for short substrings that have been stable in Steam's
// templates for years.
bool looks_like_gcpd_page (std::string_view html);   // GCPD HTML detected
bool looks_like_login_page(std::string_view html);   // session expired -> needs re-login

}  // namespace sam::steam_gcpd
