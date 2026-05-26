#pragma once

#include <string>

namespace sam::ui::widgets {

// Password input with show/hide toggle and a simple strength meter.
// Returns true if the password changed.
bool draw_password_field(const char* label, std::string& password,
                          bool show_strength = true, float width = -1.0F);

}  // namespace sam::ui::widgets
