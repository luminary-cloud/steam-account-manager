#include "ui/widgets/avatar.hpp"

#include <imgui.h>

#include "ui/widgets/avatar_cache.hpp"

namespace sam::ui::widgets {

void draw_avatar(const core::Account& a, float size) {
    const float rounding = size * (8.0F / 48.0F);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cursor, ImVec2(cursor.x + size, cursor.y + size),
                      IM_COL32(40, 50, 60, 180), rounding);
    if (auto* srv = avatar_for(a.web.avatar_url_full)) {
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv),
                            cursor, ImVec2(cursor.x + size, cursor.y + size),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, rounding);
    }
}

}  // namespace sam::ui::widgets
