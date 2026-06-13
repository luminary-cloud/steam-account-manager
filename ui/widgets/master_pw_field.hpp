#pragma once

#include <string>

namespace sam::ui::widgets {

// Returns true if the password changed.
bool draw_password_field(const char* label, std::string& password,
                          bool show_strength = true, float width = -1.0F);

// Re-mask the field. Call when swapping the form to a different account so a
// previous "show" doesn't leak across.
void reset_password_visibility(const char* label);

}  // namespace sam::ui::widgets
