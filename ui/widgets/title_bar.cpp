#include "ui/widgets/title_bar.hpp"

#include <windows.h>

#include <imgui.h>

#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"

namespace sam::ui::widgets {

namespace {

constexpr char kAppName[] = "Steam Account Manager";

}  // namespace

void draw_title_bar(app::AppState& state) {
    const HWND hwnd = state.main_hwnd;
    const float full_w = ImGui::GetContentRegionAvail().x;

    ImGui::BeginChild("##titlebar", ImVec2(full_w, kTitleBarHeight), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 end{pos.x + full_w, pos.y + kTitleBarHeight};
    const float cy = pos.y + kTitleBarHeight * 0.5F;

    draw->AddRectFilled(pos, end, ImColor(theme::panel()));
    draw->AddLine(ImVec2(pos.x, end.y - 1), ImVec2(end.x, end.y - 1), ImColor(theme::border()));

    float text_x = pos.x + 14;
    if (const ImTextureID app_ico = icons::app_icon()) {
        constexpr float kIconSize = 18.0F;
        draw->AddImage(app_ico, ImVec2(pos.x + 12, cy - kIconSize * 0.5F),
                       ImVec2(pos.x + 12 + kIconSize, cy + kIconSize * 0.5F));
        text_x = pos.x + 12 + kIconSize + 8;
    }

    if (auto* tf = fonts::title()) {
        ImGui::PushFont(tf, 0.0F);
    }
    const float text_h = ImGui::GetTextLineHeight();
    draw->AddText(ImVec2(text_x, pos.y + (kTitleBarHeight - text_h) * 0.5F),
                  ImColor(theme::text()), kAppName);
    if (fonts::title()) {
        ImGui::PopFont();
    }

    // `slot` counts in from the right edge: 1 = close, 2 = maximize, 3 = minimize.
    auto button = [&](const char* id, int slot, ImU32 hover_col, bool& hovered) {
        const float x1 = full_w - kCaptionBtnWidth * static_cast<float>(slot - 1);
        const float x0 = x1 - kCaptionBtnWidth;
        ImGui::SetCursorPos(ImVec2(x0, 0));
        ImGui::PushID(id);
        const bool clicked = ImGui::InvisibleButton("##cap", ImVec2(kCaptionBtnWidth, kTitleBarHeight));
        hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) {
            draw->AddRectFilled(ImVec2(pos.x + x0, pos.y), ImVec2(pos.x + x1, end.y), hover_col);
        }
        return clicked;
    };

    const ImU32 light = ImColor(1.0F, 1.0F, 1.0F, 0.08F);
    auto glyph_col = [](bool hovered) {
        return ImColor(hovered ? theme::text() : theme::dim_text());
    };

    bool hovered = false;

    const float min_cx = pos.x + full_w - kCaptionBtnWidth * 2.5F;
    if (button("min", 3, light, hovered) && hwnd) {
        ShowWindow(hwnd, SW_MINIMIZE);
    }
    draw->AddLine(ImVec2(min_cx - 5, cy), ImVec2(min_cx + 5, cy), glyph_col(hovered), 1.0F);

    const bool maximized = hwnd && IsZoomed(hwnd);
    const float max_cx = pos.x + full_w - kCaptionBtnWidth * 1.5F;
    if (button("max", 2, light, hovered) && hwnd) {
        ShowWindow(hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
    }
    {
        const ImU32 c = glyph_col(hovered);
        if (maximized) {
            draw->AddRect(ImVec2(max_cx - 2, cy - 5), ImVec2(max_cx + 5, cy + 2), c);
            draw->AddRect(ImVec2(max_cx - 5, cy - 2), ImVec2(max_cx + 2, cy + 5), c);
        } else {
            draw->AddRect(ImVec2(max_cx - 5, cy - 5), ImVec2(max_cx + 5, cy + 5), c);
        }
    }

    const float cls_cx = pos.x + full_w - kCaptionBtnWidth * 0.5F;
    if (button("close", 1, ImColor(theme::danger()), hovered) && hwnd) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    {
        const ImU32 c = glyph_col(hovered);
        draw->AddLine(ImVec2(cls_cx - 5, cy - 5), ImVec2(cls_cx + 5, cy + 5), c, 1.2F);
        draw->AddLine(ImVec2(cls_cx - 5, cy + 5), ImVec2(cls_cx + 5, cy - 5), c, 1.2F);
    }

    ImGui::EndChild();
}

}  // namespace sam::ui::widgets
