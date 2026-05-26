#pragma once

#include <cstdint>
#include <string_view>

namespace sam::ui::widgets {

// Draws a small rounded chip with `label` filled in `color_rgba`. Returns true if
// the chip was clicked.
bool draw_tag_chip(std::string_view label, std::uint32_t color_rgba, bool removable = false);

}  // namespace sam::ui::widgets
