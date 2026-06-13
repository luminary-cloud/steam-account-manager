#pragma once

#include <imgui.h>

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Width of the caret half of the Login split button. Callers reserve this from
// the action-row width so the remaining buttons stay equal-width and aligned.
inline constexpr float kLoginCaretWidth = 22.0F;

// Shared by the method chip fill and the caret triangle. Normal falls back to
// the default text color.
ImU32 login_method_color(core::LoginMethod m);

// No-op for Normal. Sized to the text line height so it doesn't grow the row.
void draw_login_method_chip(core::LoginMethod m);

// "Login |v" drawn as one rounded button; the caret half opens the method menu.
// Returns true if the Login half was activated.
bool draw_login_split_button(app::AppState& state, core::Account& a, float total_width);

}  // namespace sam::ui::widgets
