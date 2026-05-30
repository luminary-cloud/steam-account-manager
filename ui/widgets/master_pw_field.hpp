#pragma once

#include <string>

namespace sam::ui::widgets {

// Password input with show/hide toggle and a simple strength meter.
// Returns true if the password changed.
bool draw_password_field(const char* label, std::string& password,
                          bool show_strength = true, float width = -1.0F);

// Force the given field back to masked. Call when the form's content is swapped
// to a different account so a previous "show" doesn't leak across.
void reset_password_visibility(const char* label);

}  // namespace sam::ui::widgets
