#pragma once

#include <string>

#include "core/account_store/account.hpp"
#include "core/http/response.hpp"

namespace sam::steam_gcpd {

enum class Tab {
    Matchmaking,   // premier rating, wingman rank, active cooldown
    AccountMain,   // CS2 in-game player level + XP
};

const char* tab_to_string(Tab t);

// Authenticated GET of the GCPD page for `account`, app 730. The HTTP status is
// included so callers can detect login redirects (302 = session expired).
http::Response fetch_gcpd(const core::Account& account, Tab tab);

}  // namespace sam::steam_gcpd
