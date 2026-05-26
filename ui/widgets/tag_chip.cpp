#include "ui/widgets/tag_chip.hpp"

#include <string>

#include <imgui.h>

namespace sam::ui::widgets {

bool draw_tag_chip(std::string_view label, std::uint32_t color_rgba, bool removable) {
    const std::string text(label);
    const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pad{8, 3};
    const float extra = removable ? 14.0F : 0.0F;
    const ImVec2 box{sz.x + pad.x * 2.0F + extra, sz.y + pad.y * 2.0F};

    ImGui::PushID(text.c_str());
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##chip", box);
    const bool clicked = ImGui::IsItemClicked();

    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = IM_COL32((color_rgba >> 24) & 0xFF,
                                  (color_rgba >> 16) & 0xFF,
                                  (color_rgba >> 8) & 0xFF,
                                  64);
    const ImU32 edge = IM_COL32((color_rgba >> 24) & 0xFF,
                                  (color_rgba >> 16) & 0xFF,
                                  (color_rgba >> 8) & 0xFF,
                                  160);
    dl->AddRectFilled(cursor, ImVec2(cursor.x + box.x, cursor.y + box.y), fill, 10.0F);
    dl->AddRect(cursor, ImVec2(cursor.x + box.x, cursor.y + box.y), edge, 10.0F);
    dl->AddText(ImVec2(cursor.x + pad.x, cursor.y + pad.y), IM_COL32(240, 240, 240, 240),
                text.c_str());

    if (removable) {
        const ImVec2 x_pos{cursor.x + box.x - 14.0F + 2.0F, cursor.y + pad.y};
        dl->AddText(x_pos, IM_COL32(220, 220, 220, 180), "x");
    }

    ImGui::PopID();
    return clicked;
}

}  // namespace sam::ui::widgets
