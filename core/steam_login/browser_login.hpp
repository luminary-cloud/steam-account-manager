#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/steam_login/session.hpp"

namespace sam::steam_login {

// Builds a self-submitting HTML page that POSTs each transfer target into a
// hidden iframe to set its per-domain cookies, then navigates to `final_url`.
// Requires a context allowing cross-domain ("third-party") cookies: a normal /
// dedicated-profile window, NOT incognito (which blocks them, leaving the user
// signed out). The dedicated-profile launcher provides that.
std::string build_login_html(const std::vector<TransferTarget>& targets,
                             std::uint64_t steam_id_64,
                             const std::string& final_url);

}  // namespace sam::steam_login
