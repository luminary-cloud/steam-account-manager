#pragma once

#include <imgui.h>

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Width of the caret half of the Login split button. Callers reserve this from
// the action-row width so the remaining buttons stay equal-width and aligned.
inline constexpr float kLoginCaretWidth = 22.0F;

// Color shared by the method chip fill and the caret triangle, so the selector
// arrow matches the indicator. Normal falls back to the default text color.
ImU32 login_method_color(core::LoginMethod m);

// Compact inline indicator chip ("CS2" / "CS2+GS") drawn at the cursor. No-op
// for Normal. Sized to the text line height so it does not grow the current
// row; hovering shows a tooltip describing what Login will do.
void draw_login_method_chip(core::LoginMethod m);

// Unified "Login |v" split button of the given total width (the Login half plus
// a kLoginCaretWidth caret half), drawn as one rounded button. The caret half
// opens a menu to pick the login method and choose/update the gamesense loader.
// Returns true if the Login half was activated.
bool draw_login_split_button(app::AppState& state, core::Account& a, float total_width);

}  // namespace sam::ui::widgets
