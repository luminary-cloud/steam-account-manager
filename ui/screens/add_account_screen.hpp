#pragma once

#include "app/app_state.hpp"

namespace sam::ui::screens {

void draw_add_account(app::AppState& state);

// Hosted on the Accounts screen so they render in both list and grid mode.
// Triggered by AppState::notes_edit_requested / account_edit_requested.
void draw_edit_notes_modal(app::AppState& state);
void draw_edit_account_modal(app::AppState& state);

}  // namespace sam::ui::screens
