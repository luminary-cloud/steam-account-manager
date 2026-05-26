#include "ui/widgets/rail_nav.hpp"

#include <array>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/version.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/status_markers.hpp"

namespace sam::ui::widgets {

namespace {

struct NavEntry {
    const char* label;
    app::Screen screen;
    int (*badge_fn)(const app::AppState&) = nullptr;
};

int pending_confirmations_badge(const app::AppState& state) {
    return state.pending_confirmations_count.load(std::memory_order_relaxed);
}

constexpr float kSidebarWidth    = 188.0F;
constexpr float kSidebarPaddingX = 16.0F;
constexpr float kSidebarPaddingY = 18.0F;
constexpr float kNavItemHeight   = 32.0F;
constexpr float kNavItemSpacing  = 4.0F;
constexpr float kSectionGap      = 18.0F;
constexpr float kSidebarFooterH  = 56.0F;

bool sidebar_item(const char* label, bool selected, float width, int badge_count) {
    const ImVec2 size{width, kNavItemHeight};
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    ImGui::InvisibleButton("##nav", size);
    bool pressed = ImGui::IsItemActivated();
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    auto* draw = ImGui::GetWindowDrawList();
    if (selected) {
        draw->AddRectFilled(cursor, ImVec2(cursor.x + size.x, cursor.y + size.y),
                            ImColor(255, 255, 255, 14), 6.0F);
        const ImVec4 accent = theme::accent();
        draw->AddRectFilled(ImVec2(cursor.x, cursor.y + 6),
                            ImVec2(cursor.x + 3, cursor.y + size.y - 6),
                            ImColor(accent.x, accent.y, accent.z, 1.0F), 1.5F);
    } else if (hovered) {
        draw->AddRectFilled(cursor, ImVec2(cursor.x + size.x, cursor.y + size.y),
                            ImColor(255, 255, 255, 8), 6.0F);
    }

    const ImVec4 col = (selected || hovered) ? theme::text() : theme::dim_text();
    draw->AddText(ImVec2(cursor.x + 14, cursor.y + 8), ImColor(col), label);

    if (badge_count > 0) {
        const float by = cursor.y + size.y * 0.5F;
        const float bx = cursor.x + size.x - kMarkerSize - 8.0F;
        draw_marker(draw, ImVec2(bx, by), MarkerKind::UnreadEvent,
                    static_cast<std::size_t>(badge_count));
    }

    return pressed;
}

constexpr std::array<NavEntry, 3> kWorkspaceItems{{
    {"Accounts",      app::Screen::Accounts,      nullptr},
    {"Authenticator", app::Screen::Authenticator, nullptr},
    {"Confirmations", app::Screen::Confirmations, pending_confirmations_badge},
}};
constexpr std::array<NavEntry, 2> kManageItems{{
    {"Add account", app::Screen::AddAccount},
    {"Settings",    app::Screen::Settings},
}};

}  // namespace

void draw_rail_nav(app::AppState& state) {
    const float full_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##nav", ImVec2(kSidebarWidth, full_h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    auto* draw = ImGui::GetWindowDrawList();
    ImVec2 win_pos = ImGui::GetWindowPos();
    ImVec2 win_size{kSidebarWidth, full_h};
    draw->AddRectFilled(win_pos, ImVec2(win_pos.x + win_size.x, win_pos.y + win_size.y),
                        ImColor(0.047F, 0.047F, 0.047F, 1.0F));
    draw->AddLine(ImVec2(win_pos.x + win_size.x - 1, win_pos.y),
                  ImVec2(win_pos.x + win_size.x - 1, win_pos.y + win_size.y),
                  ImColor(0.659F, 0.635F, 0.620F, 0.10F));

    ImGui::Dummy(ImVec2(0, kSidebarPaddingY));

    if (auto* tf = fonts::title()) {
        ImGui::PushFont(tf, 0.0F);
    }
    auto centered_line = [](const char* s) {
        float w = ImGui::CalcTextSize(s).x;
        ImGui::SetCursorPosX((kSidebarWidth - w) * 0.5F);
        ImGui::TextUnformatted(s);
    };
    centered_line("Steam Account");
    centered_line("Manager");
    if (fonts::title()) {
        ImGui::PopFont();
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImVec2 sep_a = ImGui::GetCursorScreenPos();
    sep_a.x = win_pos.x + kSidebarPaddingX;
    draw->AddLine(sep_a, ImVec2(win_pos.x + win_size.x - kSidebarPaddingX, sep_a.y),
                  ImColor(0.659F, 0.635F, 0.620F, 0.12F));

    auto draw_section = [&](const char* heading, const auto& items) {
        ImGui::Dummy(ImVec2(0, kSectionGap));
        ImGui::Indent(kSidebarPaddingX);
        ImGui::TextColored(theme::dim_text(), "%s", heading);
        ImGui::Unindent(kSidebarPaddingX);
        ImGui::Dummy(ImVec2(0, 4));
        for (const auto& entry : items) {
            ImGui::SetCursorPosX(kSidebarPaddingX);
            const int badge = entry.badge_fn ? entry.badge_fn(state) : 0;
            if (sidebar_item(entry.label, state.current_screen == entry.screen,
                             kSidebarWidth - kSidebarPaddingX * 2.0F, badge)) {
                state.current_screen = entry.screen;
                if (entry.screen == app::Screen::AddAccount)
                    state.selected_account_id.clear();
                // Leaving the Accounts screen with selection mode off drops the
                // checked set so the user doesn't carry a stale selection back.
                if (entry.screen != app::Screen::Accounts &&
                    !state.selection_mode) {
                    state.selected_account_ids.clear();
                }
            }
            ImGui::Dummy(ImVec2(0, kNavItemSpacing));
        }
    };

    draw_section("Workspace", kWorkspaceItems);
    draw_section("Manage", kManageItems);

    float remaining = ImGui::GetContentRegionAvail().y;
    if (remaining > kSidebarFooterH) {
        ImGui::Dummy(ImVec2(0, remaining - kSidebarFooterH));
    }

    ImVec2 footer_sep = ImGui::GetCursorScreenPos();
    footer_sep.x = win_pos.x + kSidebarPaddingX;
    draw->AddLine(footer_sep,
                  ImVec2(win_pos.x + win_size.x - kSidebarPaddingX, footer_sep.y),
                  ImColor(0.659F, 0.635F, 0.620F, 0.12F));
    ImGui::Dummy(ImVec2(0, 10));

    char version_label[64];
    std::snprintf(version_label, sizeof(version_label), "Version: %s",
                  std::string(sam::kVersion).c_str());

    const ImTextureID gh = icons::github();
    const float icon_size = 16.0F;
    const float icon_frame_pad = 4.0F;
    const float icon_total = icon_size + icon_frame_pad * 2.0F;
    const float text_w = ImGui::CalcTextSize(version_label).x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total = icon_total + spacing + text_w;
    ImGui::SetCursorPosX((kSidebarWidth - total) * 0.5F);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(icon_frame_pad, icon_frame_pad));
    if (gh) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.14F));
        if (ImGui::ImageButton("##gh", gh, ImVec2(icon_size, icon_size))) {
            open_url(std::string(sam::kRepoUrl));
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open repository on GitHub");
        }
    } else {
        if (ImGui::SmallButton("GitHub")) {
            open_url(std::string(sam::kRepoUrl));
        }
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::dim_text(), "%s", version_label);
    ImGui::Dummy(ImVec2(0, 12));

    ImGui::EndChild();
}

}  // namespace sam::ui::widgets
