#pragma once

#include <string>

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Renders the login inline. Under privacy_mode, draws "<hidden>" and clicking
// toggles this account's id in AppState::revealed_logins. Caller controls
// surrounding style; the widget only swaps the visible text.
void draw_login_text(app::AppState& state, const core::Account& a);

// Non-clickable "<hidden>"-or-login as an owned string, for contexts that need
// one (combo previews, SeparatorText, format args).
std::string login_label(const app::AppState& state, const core::Account& a);

}  // namespace sam::ui::widgets
