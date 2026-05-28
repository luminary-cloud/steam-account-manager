#include "ui/screens/accounts_list_view.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "core/account_store/group.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/account_context_menu.hpp"
#include "ui/widgets/account_panel.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/status_markers.hpp"

namespace sam::ui::screens {

namespace {

constexpr float kListPaneWidth   = 260.0F;
constexpr float kPaneGap         = 12.0F;
constexpr float kPaneBottomInset = 36.0F;
constexpr float kPaneRightInset  = 24.0F;
constexpr float kRowHeight       = 22.0F;
constexpr float kDotRadius       = 5.0F;
constexpr float kIndent          = 10.0F;
constexpr const char* kDragPayload = "SAM_ACCOUNT_ID";

std::unordered_map<std::string, bool> g_collapsed;
std::string g_rename_target;
bool g_rename_loaded = false;

std::string make_group_id() {
    static std::atomic<std::uint64_t> counter{0};
    const auto n = counter.fetch_add(1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "g%016llx%08llx",
                  static_cast<unsigned long long>(now_seconds()),
                  static_cast<unsigned long long>(n));
    return buf;
}

std::uint32_t pack_rgba(const float c[4]) {
    auto to_byte = [](float f) {
        if (f < 0.0F) f = 0.0F;
        if (f > 1.0F) f = 1.0F;
        return static_cast<std::uint32_t>(f * 255.0F + 0.5F);
    };
    return (to_byte(c[0]) << 24) | (to_byte(c[1]) << 16) |
           (to_byte(c[2]) << 8)  |  to_byte(c[3]);
}

void unpack_rgba(std::uint32_t rgba, float out[4]) {
    out[0] = ((rgba >> 24) & 0xFF) / 255.0F;
    out[1] = ((rgba >> 16) & 0xFF) / 255.0F;
    out[2] = ((rgba >> 8)  & 0xFF) / 255.0F;
    out[3] = ( rgba        & 0xFF) / 255.0F;
}

ImU32 status_dot_color(const core::Account& a) {
    switch (a.trust) {
        case core::TrustLabel::Green:  return IM_COL32(80, 200, 120, 255);
        case core::TrustLabel::Yellow: return IM_COL32(240, 200, 80, 255);
        case core::TrustLabel::Red:    return IM_COL32(220, 80, 90, 255);
        case core::TrustLabel::Unset:  break;
    }
    return IM_COL32(120, 120, 120, 200);
}

ImU32 group_dot_color(std::uint32_t rgba) {
    return IM_COL32((rgba >> 24) & 0xFF,
                    (rgba >> 16) & 0xFF,
                    (rgba >> 8)  & 0xFF,
                    255);
}

bool& collapsed_for(const std::string& key) {
    auto [it, _] = g_collapsed.try_emplace(key, true);
    return it->second;
}

std::string truncate_to_width(const std::string& text, float max_w) {
    if (max_w <= 0.0F) return {};
    if (ImGui::CalcTextSize(text.c_str()).x <= max_w) return text;
    const float ellipsis_w = ImGui::CalcTextSize("...").x;
    std::string out = text;
    while (!out.empty()) {
        while (!out.empty()) {
            const unsigned char c = static_cast<unsigned char>(out.back());
            out.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
        if (out.empty()) break;
        if (ImGui::CalcTextSize(out.c_str()).x + ellipsis_w <= max_w) break;
    }
    out += "...";
    return out;
}

void draw_dot(const ImVec2& center, ImU32 col) {
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, kDotRadius, col, 16);
    dl->AddCircle(center, kDotRadius, IM_COL32(0, 0, 0, 80), 16, 1.0F);
}

bool draw_group_header(const std::string& key,
                       const char* label,
                       std::uint32_t color_rgba,
                       std::size_t count,
                       bool& open,
                       bool& right_clicked,
                       std::string& dropped_account_id) {
    ImGui::PushID(key.empty() ? "##ungrouped" : key.c_str());
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float row_w = ImGui::GetContentRegionAvail().x;
    const ImVec2 box{row_w, kRowHeight};
    const bool clicked = ImGui::InvisibleButton("##header", box);
    const bool hovered = ImGui::IsItemHovered();
    right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const bool drop_target = ImGui::BeginDragDropTarget();
    bool dropped_here = false;
    if (drop_target) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(kDragPayload)) {
            if (payload->Data && payload->DataSize > 0) {
                dropped_account_id.assign(
                    static_cast<const char*>(payload->Data),
                    static_cast<std::size_t>(payload->DataSize));
                dropped_here = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    auto* dl = ImGui::GetWindowDrawList();
    if (hovered || drop_target) {
        dl->AddRectFilled(cursor, ImVec2(cursor.x + box.x, cursor.y + box.y),
                          IM_COL32(255, 255, 255, drop_target ? 22 : 12), 4.0F);
    }

    const float center_y  = cursor.y + box.y * 0.5F;
    const ImU32 chevron_col = IM_COL32(200, 200, 200, 200);
    if (open) {
        const float cx = cursor.x + 10.0F;
        dl->AddTriangleFilled(ImVec2(cx - 4.0F, center_y - 2.0F),
                              ImVec2(cx + 4.0F, center_y - 2.0F),
                              ImVec2(cx,        center_y + 3.0F),
                              chevron_col);
    } else {
        const float cx = cursor.x + 8.0F;
        dl->AddTriangleFilled(ImVec2(cx, center_y - 4.0F),
                              ImVec2(cx, center_y + 4.0F),
                              ImVec2(cx + 5.0F, center_y),
                              chevron_col);
    }

    draw_dot(ImVec2(cursor.x + 24.0F, center_y), group_dot_color(color_rgba));

    const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(ImVec2(cursor.x + 38.0F, center_y - ImGui::GetTextLineHeight() * 0.5F),
                text_col, label);

    char count_buf[16];
    std::snprintf(count_buf, sizeof(count_buf), "(%zu)", count);
    const ImVec2 cs = ImGui::CalcTextSize(count_buf);
    dl->AddText(ImVec2(cursor.x + box.x - cs.x - 10.0F,
                       center_y - ImGui::GetTextLineHeight() * 0.5F),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), count_buf);

    if (clicked) open = !open;

    ImGui::PopID();
    return dropped_here;
}

void draw_account_row(app::AppState& state, const core::Account& a) {
    ImGui::PushID(a.id.c_str());

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float row_w = ImGui::GetContentRegionAvail().x;
    const ImVec2 box{row_w, kRowHeight};

    const bool is_selected_view = state.selected_account_id == a.id;
    const bool is_checked = state.is_selected(a.id);

    const bool clicked = ImGui::InvisibleButton("##row", box);
    const bool hovered = ImGui::IsItemHovered();
    const bool right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
        ImGui::SetDragDropPayload(kDragPayload, a.id.data(), a.id.size());
        ImGui::TextUnformatted(widgets::login_label(state, a).c_str());
        ImGui::EndDragDropSource();
    }

    auto* dl = ImGui::GetWindowDrawList();
    if (is_selected_view) {
        const ImVec4 accent = theme::accent();
        dl->AddRectFilled(cursor, ImVec2(cursor.x + box.x, cursor.y + box.y),
                          IM_COL32(static_cast<int>(accent.x * 255),
                                   static_cast<int>(accent.y * 255),
                                   static_cast<int>(accent.z * 255),
                                   40), 4.0F);
    } else if (hovered) {
        dl->AddRectFilled(cursor, ImVec2(cursor.x + box.x, cursor.y + box.y),
                          IM_COL32(255, 255, 255, 8), 4.0F);
    }

    const float dot_x = cursor.x + kIndent + 6.0F;
    const float center_y = cursor.y + box.y * 0.5F;
    if (state.selection_mode) {
        const ImU32 fill = is_checked
            ? group_dot_color(0x4F8EF7FFu)
            : IM_COL32(60, 60, 60, 220);
        dl->AddCircleFilled(ImVec2(dot_x, center_y), kDotRadius + 1.0F, fill, 16);
        dl->AddCircle(ImVec2(dot_x, center_y), kDotRadius + 1.0F,
                      IM_COL32(255, 255, 255, 80), 16, 1.0F);
        if (is_checked) {
            const float r = kDotRadius - 1.5F;
            const ImU32 col = IM_COL32_WHITE;
            dl->AddLine(ImVec2(dot_x - r, center_y - r),
                        ImVec2(dot_x + r, center_y + r), col, 1.5F);
            dl->AddLine(ImVec2(dot_x + r, center_y - r),
                        ImVec2(dot_x - r, center_y + r), col, 1.5F);
        }
    } else {
        draw_dot(ImVec2(dot_x, center_y), status_dot_color(a));
    }

    const std::string label_full = !a.web.persona_name.empty()
        ? a.web.persona_name
        : widgets::login_label(state, a);
    const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);

    const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const bool show_cooldown_marker =
        state.settings.list_view.show_cooldown_marker &&
        a.cs2.cooldown_expires_unix > now_unix;
    const std::size_t unread = (state.settings.notifications.enabled &&
                                 state.settings.notifications.surface_in_card &&
                                 state.settings.list_view.show_unread_badge)
        ? state.notifications.unacked_count_for(a.id) : 0;

    const float marker_gap = 8.0F;
    float marker_reserve = 0.0F;
    if (show_cooldown_marker) marker_reserve += widgets::kMarkerSize + marker_gap;
    if (unread > 0)           marker_reserve += widgets::kMarkerSize + marker_gap;

    const float label_x      = cursor.x + kIndent + 20.0F;
    const float row_right_x  = cursor.x + box.x - 10.0F - marker_reserve;
    const float total_avail  = row_right_x - label_x;

    constexpr float kGap         = 16.0F;
    constexpr float kMinLabelW   = 40.0F;

    std::string display_secondary;
    float secondary_w = 0.0F;
    if (!a.web.persona_name.empty()) {
        display_secondary = widgets::login_label(state, a);
        secondary_w = ImGui::CalcTextSize(display_secondary.c_str()).x;
        if (secondary_w + kGap + kMinLabelW > total_avail) {
            display_secondary.clear();
            secondary_w = 0.0F;
        }
    }

    const float label_max_w = secondary_w > 0.0F
        ? total_avail - secondary_w - kGap
        : total_avail;
    const std::string display_label = truncate_to_width(label_full, label_max_w);

    dl->AddText(ImVec2(label_x, center_y - ImGui::GetTextLineHeight() * 0.5F),
                text_col, display_label.c_str());

    if (!display_secondary.empty()) {
        dl->AddText(ImVec2(row_right_x - secondary_w,
                           center_y - ImGui::GetTextLineHeight() * 0.5F),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    display_secondary.c_str());
    }

    float marker_x = cursor.x + box.x - 10.0F - widgets::kMarkerSize * 0.5F;
    if (show_cooldown_marker) {
        widgets::draw_marker(dl, ImVec2(marker_x, center_y),
                              widgets::MarkerKind::Cooldown);
        marker_x -= widgets::kMarkerSize + marker_gap;
    }
    if (unread > 0) {
        widgets::draw_marker(dl, ImVec2(marker_x, center_y),
                              widgets::MarkerKind::UnreadEvent, unread);
    }

    if (clicked) {
        if (state.selection_mode) {
            state.toggle_selected(a.id);
        } else {
            state.selected_account_id = a.id;
        }
    }

    if (right_clicked && !state.selection_mode) {
        ImGui::OpenPopup("##acc-ctx");
    }
    if (begin_styled_popup("##acc-ctx")) {
        widgets::draw_account_context_menu(state, a);
        end_styled_popup();
    }

    ImGui::PopID();
}

void draw_rename_group_modal(app::AppState& state) {
    static std::array<char, 64> name_buf{};
    static float color[4]{};

    if (begin_styled_modal("Rename group", 360.0F)) {
        core::Group* g = nullptr;
        for (auto& candidate : state.vault.groups) {
            if (candidate.id == g_rename_target) { g = &candidate; break; }
        }
        if (!g) {
            ImGui::TextDisabled("Group no longer exists.");
            if (action_button("Close", ImVec2(80, 0))) {
                g_rename_loaded = false;
                g_rename_target.clear();
                ImGui::CloseCurrentPopup();
            }
            end_styled_modal();
            return;
        }
        if (!g_rename_loaded) {
            std::snprintf(name_buf.data(), name_buf.size(), "%s", g->name.c_str());
            unpack_rgba(g->color_rgba, color);
            g_rename_loaded = true;
        }

        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("##rename-name", name_buf.data(), name_buf.size());
        ImGui::Spacing();
        ImGui::TextUnformatted("Colour");
        ImGui::ColorEdit4("##rename-color", color,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

        ImGui::Spacing();
        const bool name_ok = name_buf[0] != '\0';
        ImGui::BeginDisabled(!name_ok);
        if (action_button("Save", ImVec2(100, 0))) {
            g->name = name_buf.data();
            g->color_rgba = pack_rgba(color);
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            g_rename_loaded = false;
            g_rename_target.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) {
            g_rename_loaded = false;
            g_rename_target.clear();
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }
}

}  // namespace

void draw_new_group_modal(app::AppState& state) {
    static std::array<char, 64> name_buf{};
    static float color[4] = {0.31F, 0.56F, 0.97F, 1.0F};

    if (begin_styled_modal("New group", 360.0F)) {
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("##group-name", name_buf.data(), name_buf.size());

        ImGui::Spacing();
        ImGui::TextUnformatted("Colour");
        ImGui::ColorEdit4("##group-color", color,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

        ImGui::Spacing();
        const bool name_ok = name_buf[0] != '\0';
        ImGui::BeginDisabled(!name_ok);
        if (action_button("Create", ImVec2(100, 0))) {
            core::Group g;
            g.id = make_group_id();
            g.name = name_buf.data();
            g.color_rgba = pack_rgba(color);
            state.vault.groups.push_back(std::move(g));
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            name_buf.fill('\0');
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) {
            name_buf.fill('\0');
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }
}

ListViewResult draw_list_body(app::AppState& state,
                              std::span<const std::size_t> visible) {
    ListViewResult result;

    std::unordered_set<std::string> known_group_ids;
    known_group_ids.reserve(state.vault.groups.size());
    for (const auto& g : state.vault.groups) known_group_ids.insert(g.id);

    std::unordered_map<std::string, std::vector<std::size_t>> buckets;
    std::vector<std::size_t> ungrouped;
    for (std::size_t idx : visible) {
        const auto& a = state.vault.accounts[idx];
        if (!a.group_id.empty() && known_group_ids.count(a.group_id) != 0) {
            buckets[a.group_id].push_back(idx);
        } else {
            ungrouped.push_back(idx);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel());
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));

    ImGui::BeginChild("##groups-pane",
                      ImVec2(kListPaneWidth, -kPaneBottomInset),
                      ImGuiChildFlags_Borders);

    std::string group_to_delete;
    std::string group_to_rename;

    auto render_bucket = [&](const std::string& key,
                              const char* label,
                              std::uint32_t color_rgba,
                              const std::vector<std::size_t>& indices,
                              bool can_manage) {
        bool& open = collapsed_for(key);
        bool right_clicked = false;
        std::string dropped_id;
        const bool dropped = draw_group_header(key, label, color_rgba,
                                                indices.size(), open, right_clicked,
                                                dropped_id);
        if (dropped && !dropped_id.empty()) {
            for (auto& acc : state.vault.accounts) {
                if (acc.id == dropped_id) {
                    acc.group_id = key;
                    state.vault_dirty = true;
                    state.save_vault_if_dirty();
                    break;
                }
            }
        }

        if (right_clicked && can_manage) {
            ImGui::OpenPopup(("##group-ctx-" + key).c_str());
        }
        if (begin_styled_popup(("##group-ctx-" + key).c_str())) {
            if (ImGui::MenuItem("Rename...")) group_to_rename = key;
            if (ImGui::MenuItem("Delete"))    group_to_delete = key;
            end_styled_popup();
        }

        if (open) {
            ImGui::Indent(kIndent);
            if (indices.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
                ImGui::TextUnformatted("  (empty)");
                ImGui::PopStyleColor();
            } else {
                for (std::size_t idx : indices) {
                    draw_account_row(state, state.vault.accounts[idx]);
                }
            }
            ImGui::Unindent(kIndent);
            ImGui::Dummy(ImVec2(0.0F, 2.0F));
        }
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0F));
    for (const auto& g : state.vault.groups) {
        auto it = buckets.find(g.id);
        const auto& indices = (it != buckets.end()) ? it->second
                                                    : std::vector<std::size_t>{};
        render_bucket(g.id, g.name.c_str(), g.color_rgba, indices, true);
    }
    render_bucket(std::string{}, "Ungrouped", 0x808080FFu, ungrouped, false);
    ImGui::PopStyleVar();

    if (!group_to_delete.empty()) {
        for (auto& acc : state.vault.accounts) {
            if (acc.group_id == group_to_delete) acc.group_id.clear();
        }
        state.vault.groups.erase(
            std::remove_if(state.vault.groups.begin(), state.vault.groups.end(),
                [&](const core::Group& g) { return g.id == group_to_delete; }),
            state.vault.groups.end());
        state.vault_dirty = true;
        state.save_vault_if_dirty();
    }
    if (!group_to_rename.empty()) {
        g_rename_target = group_to_rename;
        g_rename_loaded = false;
        ImGui::OpenPopup("Rename group");
    }
    draw_rename_group_modal(state);

    ImGui::EndChild();   // ##groups-pane

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    ImGui::SameLine(0.0F, kPaneGap);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel());
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));

    ImGui::BeginChild("##detail-pane",
                      ImVec2(-kPaneRightInset, -kPaneBottomInset),
                      ImGuiChildFlags_Borders);

    core::Account* selected = nullptr;
    if (!state.selected_account_id.empty()) {
        selected = state.find_account(state.selected_account_id);
    }
    if (selected != nullptr) {
        bool still_visible = false;
        for (std::size_t idx : visible) {
            if (state.vault.accounts[idx].id == selected->id) {
                still_visible = true;
                break;
            }
        }
        if (!still_visible) selected = nullptr;
    }

    if (selected == nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextWrapped("Pick an account on the left to see its details.");
        ImGui::PopStyleColor();
    } else {
        const auto act = widgets::draw_account_panel(state, *selected);
        if (act != widgets::CardAction::None) {
            result.action = act;
            result.action_account_id = selected->id;
        }
    }

    ImGui::EndChild();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    return result;
}

}  // namespace sam::ui::screens
