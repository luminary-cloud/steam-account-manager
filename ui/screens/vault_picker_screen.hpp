#pragma once

#include "app/app_state.hpp"

namespace sam::ui::screens {

// Startup vault chooser, shown (instead of the unlock screen) when several vaults
// exist and none is set to auto-open. Selecting a vault makes it active and either
// unlocks it silently (if it has a cache) or hands off to the unlock screen.
void draw_vault_picker(app::AppState& state);

}  // namespace sam::ui::screens
