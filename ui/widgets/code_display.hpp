#pragma once

#include <cstdint>
#include <string_view>

namespace sam::ui::widgets {

void draw_code_display(std::string_view code, int seconds_left);

}  // namespace sam::ui::widgets
