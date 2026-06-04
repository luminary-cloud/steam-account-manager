#include "ui/widgets/status_markers.hpp"

#include <cstdio>

#include "ui/theme.hpp"

namespace sam::ui::widgets {

namespace {

ImU32 marker_color(MarkerKind k) {
    ImVec4 c;
    switch (k) {
        case MarkerKind::UnreadEvent: c = theme::danger();  break;
        case MarkerKind::Cooldown:    c = theme::warning(); break;
        case MarkerKind::WeeklyDrop:  c = theme::success(); break;
    }
    return IM_COL32(static_cast<int>(c.x * 255),
                    static_cast<int>(c.y * 255),
                    static_cast<int>(c.z * 255), 235);
}

}  // namespace

void draw_marker(ImDrawList* dl, ImVec2 center, MarkerKind kind, std::size_t count) {
    if (!dl) return;
    const float r = kMarkerSize * 0.5F;
    const ImU32 fill = marker_color(kind);
    dl->AddCircleFilled(center, r, fill, 18);
    dl->AddCircle(center, r, IM_COL32(0, 0, 0, 120), 18, 1.0F);

    if (kind == MarkerKind::WeeklyDrop) {
        // The default font has no checkmark glyph, so stroke one in.
        dl->AddLine(ImVec2(center.x - 3.0F, center.y + 0.2F),
                    ImVec2(center.x - 0.8F, center.y + 2.6F), IM_COL32_WHITE, 1.7F);
        dl->AddLine(ImVec2(center.x - 0.8F, center.y + 2.6F),
                    ImVec2(center.x + 3.4F, center.y - 2.8F), IM_COL32_WHITE, 1.7F);
        return;
    }

    const char* glyph = "!";
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(center.x - ts.x * 0.5F, center.y - ts.y * 0.5F),
                IM_COL32_WHITE, glyph);

    if (kind == MarkerKind::UnreadEvent && count > 1) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%zu", count);
        const ImVec2 cs = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(center.x + r + 2.0F, center.y - cs.y * 0.5F),
                    IM_COL32(255, 255, 255, 200), buf);
    }
}

}  // namespace sam::ui::widgets
