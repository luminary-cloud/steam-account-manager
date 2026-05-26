#include "ui/theme.hpp"

#include <imgui.h>

namespace sam::ui::theme {

namespace {

ImVec4 g_bg          = {0.035F, 0.035F, 0.035F, 1.0F};
ImVec4 g_panel       = {0.047F, 0.047F, 0.047F, 1.0F};
ImVec4 g_panel_hover = {0.090F, 0.090F, 0.090F, 1.0F};
ImVec4 g_text        = {0.950F, 0.960F, 0.970F, 1.0F};
ImVec4 g_dim_text    = {0.443F, 0.443F, 0.471F, 1.0F};
ImVec4 g_accent      = {0.310F, 0.690F, 1.000F, 1.0F};
ImVec4 g_success     = {0.360F, 0.850F, 0.550F, 1.0F};
ImVec4 g_danger      = {0.920F, 0.420F, 0.450F, 1.0F};
ImVec4 g_warning     = {0.970F, 0.790F, 0.360F, 1.0F};
ImVec4 g_border      = {0.659F, 0.635F, 0.620F, 0.10F};

}  // namespace

void apply_dark(ImVec4 accent) {
    g_accent = accent;

    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 0.0F;
    s.ChildRounding     = 6.0F;
    s.FrameRounding     = 5.0F;
    s.PopupRounding     = 6.0F;
    s.GrabRounding      = 4.0F;
    s.ScrollbarRounding = 100.0F;
    s.TabRounding       = 4.0F;

    s.WindowPadding    = ImVec2(0, 0);
    s.FramePadding     = ImVec2(8, 4);
    s.ItemSpacing      = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing    = 18.0F;
    s.ScrollbarSize    = 6.0F;

    s.WindowBorderSize = 0.0F;
    s.ChildBorderSize  = 0.0F;
    s.PopupBorderSize  = 1.0F;
    s.FrameBorderSize  = 0.0F;
    s.TabBorderSize    = 0.0F;

    auto& c = s.Colors;
    c[ImGuiCol_Text]                 = g_text;
    c[ImGuiCol_TextDisabled]         = g_dim_text;

    c[ImGuiCol_WindowBg]             = g_bg;
    c[ImGuiCol_ChildBg]              = g_panel;
    c[ImGuiCol_PopupBg]              = ImVec4(g_panel.x, g_panel.y, g_panel.z, 0.98F);
    c[ImGuiCol_Border]               = g_border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);

    c[ImGuiCol_FrameBg]              = ImVec4(0.07F, 0.07F, 0.07F, 1.00F);
    c[ImGuiCol_FrameBgHovered]       = g_panel_hover;
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.118F, 0.118F, 0.118F, 1.00F);
    c[ImGuiCol_CheckboxSelectedBg]   = ImVec4(
        c[ImGuiCol_FrameBg].x + (c[ImGuiCol_FrameBgHovered].x - c[ImGuiCol_FrameBg].x) * 0.65F,
        c[ImGuiCol_FrameBg].y + (c[ImGuiCol_FrameBgHovered].y - c[ImGuiCol_FrameBg].y) * 0.65F,
        c[ImGuiCol_FrameBg].z + (c[ImGuiCol_FrameBgHovered].z - c[ImGuiCol_FrameBg].z) * 0.65F,
        c[ImGuiCol_FrameBg].w + (c[ImGuiCol_FrameBgHovered].w - c[ImGuiCol_FrameBg].w) * 0.65F);

    c[ImGuiCol_TitleBg]              = g_bg;
    c[ImGuiCol_TitleBgActive]        = g_bg;
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(g_bg.x, g_bg.y, g_bg.z, 0.80F);
    c[ImGuiCol_MenuBarBg]            = g_panel;

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.659F, 0.635F, 0.620F, 0.20F);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.659F, 0.635F, 0.620F, 0.35F);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.659F, 0.635F, 0.620F, 0.55F);

    c[ImGuiCol_CheckMark]            = g_accent;
    c[ImGuiCol_SliderGrab]           = g_accent;
    c[ImGuiCol_SliderGrabActive]     = ImVec4(g_accent.x, g_accent.y, g_accent.z, 1.0F);

    c[ImGuiCol_Button]               = ImVec4(0.090F, 0.090F, 0.090F, 1.00F);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.118F, 0.118F, 0.118F, 1.00F);
    c[ImGuiCol_ButtonActive]         = ImVec4(g_accent.x * 0.55F, g_accent.y * 0.55F, g_accent.z * 0.55F, 1.0F);

    c[ImGuiCol_Header]               = ImVec4(0.080F, 0.080F, 0.080F, 1.00F);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.110F, 0.110F, 0.110F, 1.00F);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.140F, 0.140F, 0.140F, 1.00F);

    c[ImGuiCol_Separator]            = g_border;
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.659F, 0.635F, 0.620F, 0.20F);
    c[ImGuiCol_SeparatorActive]      = g_accent;

    c[ImGuiCol_ResizeGrip]           = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(g_accent.x, g_accent.y, g_accent.z, 0.40F);
    c[ImGuiCol_ResizeGripActive]     = g_accent;

    c[ImGuiCol_Tab]                  = ImVec4(0.047F, 0.047F, 0.047F, 1.00F);
    c[ImGuiCol_TabHovered]           = ImVec4(0.090F, 0.090F, 0.090F, 1.00F);
    c[ImGuiCol_TabSelected]          = ImVec4(0.070F, 0.070F, 0.070F, 1.00F);

    c[ImGuiCol_DockingPreview]       = ImVec4(g_accent.x, g_accent.y, g_accent.z, 0.50F);
    c[ImGuiCol_DockingEmptyBg]       = g_bg;

    c[ImGuiCol_PlotLines]            = ImVec4(0.61F, 0.61F, 0.61F, 1.00F);
    c[ImGuiCol_PlotHistogram]        = g_accent;

    c[ImGuiCol_TextSelectedBg]       = ImVec4(g_accent.x, g_accent.y, g_accent.z, 0.35F);
    c[ImGuiCol_NavCursor]            = g_accent;
}

ImVec4 bg()          { return g_bg; }
ImVec4 panel()       { return g_panel; }
ImVec4 panel_hover() { return g_panel_hover; }
ImVec4 text()        { return g_text; }
ImVec4 dim_text()    { return g_dim_text; }
ImVec4 accent()      { return g_accent; }
ImVec4 success()     { return g_success; }
ImVec4 danger()      { return g_danger; }
ImVec4 warning()     { return g_warning; }
ImVec4 border()      { return g_border; }

}  // namespace sam::ui::theme
