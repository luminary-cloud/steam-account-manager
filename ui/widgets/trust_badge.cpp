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

namespace {
// Right-aligned rounded corner pill (NFA/Cached badges). r,g,b set the tint; the
// fill/border/text alphas match the trust-badge styling.
void draw_corner_pill(float right_x, float center_y, const char* label,
                      int r, int g, int b) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    constexpr float pad_x = 5.0F;
    constexpr float pad_y = 2.0F;
    const float w = ts.x + pad_x * 2.0F;
    const float h = ts.y + pad_y * 2.0F;
    const float x0 = right_x - w;
    const float y0 = center_y - h * 0.5F;
    const float rad = h * 0.5F;  // fully rounded ends
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(right_x, y0 + h), IM_COL32(r, g, b, 0x33), rad);
    dl->AddRect(ImVec2(x0, y0), ImVec2(right_x, y0 + h), IM_COL32(r, g, b, 0xCC), rad);
    dl->AddText(ImVec2(x0 + pad_x, y0 + pad_y), IM_COL32(r, g, b, 0xFF), label);
}
}  // namespace

void draw_nfa_pill(float right_x, float center_y) {
    draw_corner_pill(right_x, center_y, "NFA", 0xF7, 0xB5, 0x4A);
}

void draw_cached_pill(float right_x, float center_y) {
    draw_corner_pill(right_x, center_y, "Cached", 0x5F, 0xC9, 0xA3);
}

void draw_revoked_pill(float right_x, float center_y) {
    draw_corner_pill(right_x, center_y, "Revoked", 0xE0, 0x4F, 0x4F);
}

}  // namespace sam::ui::widgets
