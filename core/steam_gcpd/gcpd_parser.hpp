#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sam::steam_gcpd {

// From /gcpd/730?tab=matchmaking.
struct MatchmakingData {
    bool ok = false;
    int  premier_rating = -1;
    int  premier_wins   = -1;
    int  wingman_rank   = -1;
    int  wingman_wins   = -1;
    std::int64_t cooldown_expires_unix = 0;   // 0 = no active cooldown
    std::string  cooldown_reason;
};

// From /gcpd/730?tab=accountmain. CS2 in-game level, distinct from the Web-API
// Steam profile level.
struct AccountMainData {
    bool ok = false;
    int  cs2_player_level = -1;
    int  cs2_player_xp    = -1;
};

MatchmakingData parse_matchmaking(std::string_view html);
AccountMainData parse_accountmain(std::string_view html);

// Content-signature checks distinguishing a real GCPD response from the login
// page Steam serves for an invalid session cookie.
bool looks_like_gcpd_page (std::string_view html);
bool looks_like_login_page(std::string_view html);

}  // namespace sam::steam_gcpd
