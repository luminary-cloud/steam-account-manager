#pragma once

#include "app/app_state.hpp"

namespace sam::ui::screens {

void draw_confirmations(app::AppState& state);

// Triggers the same staggered refresh-all path as the UI "Refresh all" button.
// Safe to call from the UI thread (e.g. via post_ui_callback from a worker).
void confirmations_trigger_refresh_all(app::AppState& state);

}  // namespace sam::ui::screens
