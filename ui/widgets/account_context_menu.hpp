#pragma once

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Fills the popup body only; the caller opens and ends the popup.
void draw_account_context_menu(app::AppState& state, const core::Account& a);

}  // namespace sam::ui::widgets
