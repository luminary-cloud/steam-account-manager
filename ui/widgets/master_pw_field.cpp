#include "ui/widgets/master_pw_field.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

#include <imgui.h>

#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::widgets {

namespace {

// Show/hide state per field, keyed by label. Lives here rather than as a
// function-local static so reset_password_visibility() can clear it.
std::unordered_map<std::string, bool> g_visible_map;

int score(const std::string& s) {
    if (s.empty()) return 0;
    int classes = 0;
    bool lo = false, up = false, dg = false, sy = false;
    for (char c : s) {
        if (std::islower(static_cast<unsigned char>(c))) lo = true;
        else if (std::isupper(static_cast<unsigned char>(c))) up = true;
        else if (std::isdigit(static_cast<unsigned char>(c))) dg = true;
        else sy = true;
    }
    classes = lo + up + dg + sy;
    int length_score = std::min<int>(static_cast<int>(s.size()), 24) * 4;
    return length_score + classes * 10;
}

}  // namespace

bool draw_password_field(const char* label, std::string& password,
                          bool show_strength, float width) {
    // Scope every widget in this field by `label` so two password fields on
    // the same screen (e.g. "Password" + "Shared secret") don't collide on
    // the bare "show"/"hide" toggle button id.
    ImGui::PushID(label);

    if (width > 0.0F) ImGui::SetNextItemWidth(width);

    bool& visible = g_visible_map[std::string(label)];

    std::array<char, 256> buf{};
    std::snprintf(buf.data(), buf.size(), "%s", password.c_str());

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_None;
    if (!visible) flags |= ImGuiInputTextFlags_Password;

    bool changed = false;
    if (ImGui::InputText(label, buf.data(), buf.size(), flags)) {
        password = buf.data();
        changed = true;
    }
    ImGui::SameLine();
    if (sam::ui::action_button(visible ? "hide" : "show")) {
        visible = !visible;
    }

    if (show_strength) {
        const int s = score(password);
        const float frac = std::clamp(static_cast<float>(s) / 130.0F, 0.0F, 1.0F);
        const ImVec4 color = s < 50  ? theme::danger()
                            : s < 90  ? theme::warning()
                                       : theme::success();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(frac, ImVec2(width > 0 ? width : -FLT_MIN, 4),
                           password.empty() ? "" : (s < 50 ? "weak" : s < 90 ? "ok" : "strong"));
        ImGui::PopStyleColor();
    }

    ImGui::PopID();
    return changed;
}

void reset_password_visibility(const char* label) {
    g_visible_map[std::string(label)] = false;
}

}  // namespace sam::ui::widgets
