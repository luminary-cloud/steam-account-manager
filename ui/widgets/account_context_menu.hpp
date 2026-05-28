#pragma once

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Renders the body of the right-click context menu for an account row or card:
// copy login, copy password, copy 2FA code, copy SteamID64. Caller is
// responsible for opening the popup (OpenPopup + BeginPopup / BeginPopupContextWindow)
// and ending it; this only fills the popup body.
void draw_account_context_menu(app::AppState& state, const core::Account& a);

}  // namespace sam::ui::widgets
