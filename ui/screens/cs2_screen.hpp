#pragma once

#include "app/app_state.hpp"

namespace sam::ui::screens {

void draw_cs2(app::AppState& state);

// Retires any live GC client and clears the screen state. Safe to call off the CS2
// screen (e.g. when the feature is disabled while connected); the join happens on a
// background reaper, so this never blocks the render thread.
void discard_cs2_client(app::AppState& state);

}  // namespace sam::ui::screens
