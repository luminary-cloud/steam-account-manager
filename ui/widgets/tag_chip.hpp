#pragma once

#include <cstdint>
#include <string_view>

namespace sam::ui::widgets {

bool draw_tag_chip(std::string_view label, std::uint32_t color_rgba, bool removable = false);

}  // namespace sam::ui::widgets
