#pragma once

#include "app/app_state.hpp"

namespace sam::ui::screens {

void draw_unlock(app::AppState& state);

// Attempts to open the ACTIVE vault silently using its DPAPI master-password
// cache (only if remember_master_password is on). Returns true and fully enters
// the session (bind + refresh) on success; on failure removes the stale cache and
// returns false. Shared by the startup picker so picking a cached vault skips the
// password prompt.
bool try_cached_unlock(app::AppState& state);

// Creates a brand-new vault (file + registry entry), sets it active, and enters
// the session. Returns false and fills `err` on failure. Used by the picker's and
// settings' "New vault" flows.
bool create_and_open_vault(app::AppState& state, const std::string& name,
                           std::uint32_t color, const std::string& pw,
                           std::string& err);

}  // namespace sam::ui::screens
