#pragma once

#include <string>

#include "core/account_store/account.hpp"

namespace sam::profile {

struct PersonaChangeResult {
    bool ok = false;
    std::string error;
    // The name Steam actually applied (it may trim/clean the input). Empty
    // unless the change succeeded.
    std::string applied_name;
    // True when the failure was a session/auth problem the caller can recover
    // from with a full credentials re-login (vs. a name Steam rejected).
    bool needs_relogin = false;
};

// Changes the account's Steam display name (persona name) via the community
// ajaxsetpersonaname endpoint. Ensures a usable web session first (refreshes
// the access token and mints a steamLoginSecure cookie as needed), mutating
// `a` with any refreshed tokens/cookie so the caller can persist them.
//
// Must run on a worker thread: it performs blocking network requests.
PersonaChangeResult change_persona_name(core::Account& a, const std::string& new_name);

}  // namespace sam::profile
