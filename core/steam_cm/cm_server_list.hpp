#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sam::steam_cm {

struct CmServer {
    std::string host;
    std::uint16_t port = 0;
    double load = 0.0;
};

// Fetches the websocket CM list from ISteamDirectory/GetCMListForConnect, sorted by
// ascending weighted load. Empty on failure. Uses the calling thread's proxy.
std::vector<CmServer> fetch_cm_list();

}  // namespace sam::steam_cm
