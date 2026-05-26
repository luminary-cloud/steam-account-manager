#include "ui/widgets/redacted_text.hpp"

#include <imgui.h>

#include "ui/util.hpp"

namespace sam::ui::widgets {

namespace {

constexpr const char* kHiddenLabel = "<hidden>";

bool is_redacted(const app::AppState& state, const core::Account& a) {
    return state.settings.privacy_mode &&
           state.revealed_logins.find(a.id) == state.revealed_logins.end();
}

}  // namespace

void draw_login_text(app::AppState& state, const core::Account& a) {
    const bool redacted = is_redacted(state, a);
    ImGui::TextUnformatted(redacted ? kHiddenLabel : a.login.c_str());
    if (!state.settings.privacy_mode) return;
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemClicked()) {
        if (redacted) {
            state.revealed_logins.insert(a.id);
        } else {
            state.revealed_logins.erase(a.id);
        }
    }
    hover_tooltip(redacted ? "Click to reveal this account name."
                            : "Click to hide this account name.");
}

std::string login_label(const app::AppState& state, const core::Account& a) {
    return is_redacted(state, a) ? std::string(kHiddenLabel) : a.login;
}

}  // namespace sam::ui::widgets
