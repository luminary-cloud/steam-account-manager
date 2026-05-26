#include "ui/widgets/trust_badge.hpp"

#include <imgui.h>

namespace sam::ui::widgets {

namespace {

ImU32 color_for(core::TrustLabel t) {
    switch (t) {
        case core::TrustLabel::Green:  return IM_COL32(80, 200, 120, 255);
        case core::TrustLabel::Yellow: return IM_COL32(240, 200, 80, 255);
        case core::TrustLabel::Red:    return IM_COL32(220, 80, 90, 255);
        case core::TrustLabel::Unset:  return IM_COL32(100, 100, 100, 255);
    }
    return IM_COL32(100, 100, 100, 255);
}

}  // namespace

bool draw_trust_badge(core::TrustLabel& trust, bool editable, float radius) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 box{radius * 2.0F + 2.0F, radius * 2.0F + 2.0F};
    ImGui::InvisibleButton("##trust", box);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 center{cursor.x + radius + 1.0F, cursor.y + radius + 1.0F};
    dl->AddCircleFilled(center, radius, color_for(trust), 16);
    dl->AddCircle(center, radius, IM_COL32(0, 0, 0, 80), 16, 1.0F);

    bool changed = false;
    if (editable && ImGui::IsItemActivated()) {
        const auto next = static_cast<core::TrustLabel>(
            (static_cast<int>(trust) + 1) % 4);
        trust = next;
        changed = true;
    }
    return changed;
}

}  // namespace sam::ui::widgets
