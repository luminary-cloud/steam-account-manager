#pragma once

#include "app/app_state.hpp"

namespace sam::ui::widgets {

void draw_rail_nav(app::AppState& state);

// Save + lock the current vault, record `id` as the target, relaunch the app with
// --switch (it waits for this instance's mutex to free), and close this window.
// No-op if `id` is empty or already active.
void request_vault_switch(app::AppState& state, const std::string& id);

}  // namespace sam::ui::widgets
