#pragma once

#include <string>

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Renders the account login inline. When privacy_mode is on and this
// account's id is not in AppState::revealed_logins, draws "<hidden>" as
// clickable text and a click adds the id to the reveal set. When already
// revealed (or privacy_mode is off), draws the login string; clicking it
// removes the id and re-hides. The caller controls surrounding style
// (PushStyleColor etc.) - the widget only swaps the visible text.
void draw_login_text(app::AppState& state, const core::Account& a);

// Returns "<hidden>" or the login string for contexts that need an owned
// std::string (combo previews, SeparatorText, printf-style format args).
// Not clickable - pair with draw_login_text where an interactive reveal is
// needed.
std::string login_label(const app::AppState& state, const core::Account& a);

}  // namespace sam::ui::widgets
