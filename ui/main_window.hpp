#pragma once

#include "app/app_state.hpp"

namespace sam::ui {

// Draws the full-frame UI: rail nav on the left, screen content on the right.
// Returns false if the app should quit.
bool draw(app::AppState& state);

}  // namespace sam::ui
