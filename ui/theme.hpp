#pragma once

#include <imgui.h>

namespace sam::ui::theme {

void apply_dark(ImVec4 accent = {0.31F, 0.69F, 1.00F, 1.0F});

ImVec4 bg();
ImVec4 panel();
ImVec4 panel_hover();
ImVec4 text();
ImVec4 dim_text();
ImVec4 accent();
ImVec4 success();
ImVec4 danger();
ImVec4 warning();
ImVec4 border();

}  // namespace sam::ui::theme
