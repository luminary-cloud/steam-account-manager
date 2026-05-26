#include "ui/widgets/code_display.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "ui/theme.hpp"

namespace sam::ui::widgets {

void draw_code_display(std::string_view code, int seconds_left) {
    const std::string text(code);

    const float font_size = ImGui::GetFontSize() * 2.6F;
    auto* font = ImGui::GetFont();
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0F, text.c_str());

    const float radius = 60.0F;
    const float gap = 24.0F;
    const float width = text_size.x + gap + radius * 2.0F;
    const float height = std::max(text_size.y, radius * 2.0F);

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));

    auto* dl = ImGui::GetWindowDrawList();

    dl->AddText(font, font_size,
                ImVec2(cursor.x, cursor.y + (height - text_size.y) / 2.0F),
                ImGui::ColorConvertFloat4ToU32(theme::text()),
                text.c_str());

    const ImVec2 center{cursor.x + text_size.x + gap + radius, cursor.y + height / 2.0F};
    const float t = std::clamp(static_cast<float>(seconds_left) / 30.0F, 0.0F, 1.0F);

    dl->AddCircle(center, radius - 4.0F, IM_COL32(255, 255, 255, 28), 64, 4.0F);

    const ImU32 col = seconds_left <= 5
        ? IM_COL32(220, 80, 90, 255)
        : ImGui::ColorConvertFloat4ToU32(theme::accent());

    constexpr float kPi = 3.1415926F;
    const float a0 = -kPi / 2.0F;
    const float a1 = a0 + t * kPi * 2.0F;

    dl->PathArcTo(center, radius - 4.0F, a0, a1, 64);
    dl->PathStroke(col, 4.0F);

    char buf[8];
    std::snprintf(buf, sizeof(buf), "%ds", seconds_left);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(center.x - ts.x / 2.0F, center.y - ts.y / 2.0F),
                ImGui::ColorConvertFloat4ToU32(theme::dim_text()),
                buf);
}

}  // namespace sam::ui::widgets
