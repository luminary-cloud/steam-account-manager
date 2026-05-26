#pragma once

#include <cstdint>
#include <string_view>

namespace sam::ui::widgets {

// Draws a large monospace 5-character TOTP with a circular progress arc
// representing how much of the 30-second window is left.
void draw_code_display(std::string_view code, int seconds_left);

}  // namespace sam::ui::widgets
