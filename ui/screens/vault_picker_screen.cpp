#include "ui/screens/vault_picker_screen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app_paths.hpp"
#include "app/vault_registry.hpp"
#include "platform/paths.hpp"
#include "ui/fonts.hpp"
#include "ui/screens/unlock_screen.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/master_pw_field.hpp"

namespace sam::ui::screens {

namespace {

ImU32 rgba_to_col(std::uint32_t rgba) {
    return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF,
                    (rgba >> 8) & 0xFF, rgba & 0xFF);
}

// A vault's icon: the custom image if it has one and it's decoded, else a colored
// disc with the first letter of the name. Draws centered in [center] with radius r.
void draw_vault_badge(const app::VaultInfo& v, ImVec2 center, float r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!v.icon_file.empty()) {
        const auto path = app::vault_dir_for(v.id) / std::filesystem::path(v.icon_file);
        if (auto* tex = widgets::texture_for_file(path.string())) {
            dl->AddImageRounded(reinterpret_cast<ImTextureID>(tex),
                                ImVec2(center.x - r, center.y - r),
                                ImVec2(center.x + r, center.y + r),
                                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
            return;
        }
    }
    dl->AddCircleFilled(center, r, rgba_to_col(v.color_rgba), 32);
    const char initial[2] = {
        static_cast<char>(v.name.empty() ? '?' : std::toupper((unsigned char)v.name[0])),
        '\0'};
    const ImVec2 ts = ImGui::CalcTextSize(initial);
    dl->AddText(ImVec2(center.x - ts.x / 2.0F, center.y - ts.y / 2.0F),
                IM_COL32(245, 245, 245, 255), initial);
}

// Opens the chosen vault: silently if it has a usable cache, otherwise routes to
// the unlock screen (which now targets the active vault).
void select_vault(app::AppState& state, const std::string& id) {
    sam::platform::set_active_vault_id(id);
    if (!try_cached_unlock(state)) {
        state.needs_vault_pick = false;  // fall through to draw_unlock
    }
}

}  // namespace

void draw_vault_picker(app::AppState& state) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text());
    if (ImFont* tf = fonts::title()) {
        ImGui::PushFont(tf, 0.0F);
        ImGui::TextUnformatted("Choose a vault");
        ImGui::PopFont();
    } else {
        ImGui::TextUnformatted("Choose a vault");
    }
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
    ImGui::TextUnformatted("Each vault keeps its own accounts. Settings are shared.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 12));

    // Stable display order.
    std::vector<const app::VaultInfo*> items;
    items.reserve(state.vault_registry.vaults.size());
    for (const auto& v : state.vault_registry.vaults) items.push_back(&v);
    std::sort(items.begin(), items.end(), [](auto* a, auto* b) {
        if (a->order != b->order) return a->order < b->order;
        return a->name < b->name;
    });

    const float card_w = 168.0F, card_h = 152.0F, gap = 14.0F;
    const float avail = ImGui::GetContentRegionAvail().x;
    int per_row = std::max(1, static_cast<int>((avail + gap) / (card_w + gap)));

    std::string pick;                 // vault id chosen this frame
    bool open_new = false;

    const int total = static_cast<int>(items.size()) + 1;  // +1 for the "New vault" card
    for (int i = 0; i < total; ++i) {
        if (i % per_row != 0) ImGui::SameLine(0, gap);
        ImGui::PushID(i);

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("card", ImVec2(card_w, card_h));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const ImVec2 p1(p0.x + card_w, p0.y + card_h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(
                                      hovered ? theme::panel_hover() : theme::panel()),
                          10.0F);
        if (hovered) {
            dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::accent()),
                        10.0F, 1.5F);
        }

        const ImVec2 center(p0.x + card_w / 2.0F, p0.y + 58.0F);
        if (i < static_cast<int>(items.size())) {
            const app::VaultInfo& v = *items[i];
            draw_vault_badge(v, center, 38.0F);
            const ImVec2 ts = ImGui::CalcTextSize(v.name.c_str());
            const float tx = std::clamp(p0.x + (card_w - ts.x) / 2.0F, p0.x + 6.0F,
                                        p1.x - ts.x - 6.0F);
            dl->AddText(ImVec2(tx, p0.y + card_h - 34.0F),
                        ImGui::ColorConvertFloat4ToU32(theme::text()), v.name.c_str());
            if (clicked) pick = v.id;
        } else {
            dl->AddCircleFilled(center, 38.0F, ImGui::ColorConvertFloat4ToU32(
                                                   theme::panel_hover()), 32);
            const ImVec2 ps = ImGui::CalcTextSize("+");
            dl->AddText(ImVec2(center.x - ps.x / 2.0F, center.y - ps.y / 2.0F),
                        ImGui::ColorConvertFloat4ToU32(theme::dim_text()), "+");
            const char* label = "New vault";
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(p0.x + (card_w - ts.x) / 2.0F, p0.y + card_h - 34.0F),
                        ImGui::ColorConvertFloat4ToU32(theme::dim_text()), label);
            if (clicked) open_new = true;
        }
        ImGui::PopID();
    }

    if (!pick.empty()) select_vault(state, pick);
    if (open_new) ImGui::OpenPopup("New vault");

    // ---- New vault modal ----
    static std::string new_name, new_pw, new_pw_confirm, new_error;
    if (begin_styled_modal("New vault")) {
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(320.0F);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", new_name.c_str());
        if (ImGui::InputText("##vault_name", buf, sizeof(buf))) new_name = buf;

        ImGui::Spacing();
        ImGui::TextUnformatted("Master password");
        widgets::draw_password_field("##new_vault_pw", new_pw, true, 320.0F);
        ImGui::TextUnformatted("Confirm");
        widgets::draw_password_field("##new_vault_pw_confirm", new_pw_confirm, false, 320.0F);

        if (!new_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", new_error.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        const bool ok = !new_pw.empty() && new_pw == new_pw_confirm && !new_name.empty();
        ImGui::BeginDisabled(!ok);
        if (action_button("Create", ImVec2(120, 0))) {
            if (create_and_open_vault(state, new_name, 0x4F8EF7FFu, new_pw, new_error)) {
                new_name.clear();
                new_pw.clear();
                new_pw_confirm.clear();
                new_error.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(90, 0))) {
            new_pw.clear();
            new_pw_confirm.clear();
            new_error.clear();
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }
}

}  // namespace sam::ui::screens
