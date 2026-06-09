#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/steam_login/session.hpp"

namespace sam::steam_login {

// Builds a self-submitting HTML page that signs a browser in to Steam, then
// navigates to `final_url`. It replicates Steam's own login transfer: each
// `targets` entry (steamcommunity.com, store., help., ...) is POSTed into a
// hidden iframe so the per-domain cookies are set, then the top window
// redirects to `final_url`.
//
// This needs a context where cross-domain ("third-party") cookies are allowed,
// i.e. a normal/dedicated-profile window, NOT incognito (which blocks them and
// would leave the user signed out). The dedicated-profile launcher provides
// that.
std::string build_login_html(const std::vector<TransferTarget>& targets,
                             std::uint64_t steam_id_64,
                             const std::string& final_url);

}  // namespace sam::steam_login
