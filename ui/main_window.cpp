#include "ui/main_window.hpp"

#include <imgui.h>

#include "ui/screens/accounts_screen.hpp"
#include "ui/screens/add_account_screen.hpp"
#include "ui/screens/confirmations_screen.hpp"
#include "ui/screens/sda_screen.hpp"
#include "ui/screens/settings_screen.hpp"
#include "ui/screens/unlock_screen.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/rail_nav.hpp"

namespace sam::ui {

namespace {

constexpr float kContentPaddingX = 24.0F;
constexpr float kContentPaddingY = 20.0F;

void draw_screen(app::AppState& state) {
    switch (state.current_screen) {
        case app::Screen::Unlock:        screens::draw_unlock(state);        break;
        case app::Screen::Accounts:      screens::draw_accounts(state);      break;
        case app::Screen::Authenticator: screens::draw_sda(state);           break;
        case app::Screen::Confirmations: screens::draw_confirmations(state); break;
        case app::Screen::AddAccount:    screens::draw_add_account(state);   break;
        case app::Screen::Settings:      screens::draw_settings(state);      break;
    }
}

}  // namespace

bool draw(app::AppState& state) {
    const auto vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    constexpr ImGuiWindowFlags root_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::bg());

    bool keep_running = true;
    if (ImGui::Begin("sam-root", nullptr, root_flags)) {
        if (state.unlocked) {
            // A background refresh discovered we need to re-login for an
            // account: switch to the Add Account screen so the Full Login
            // wizard can pick up the prefilled username. The wizard clears
            // the optional once it consumes it.
            if (state.pending_relogin_login.has_value() &&
                state.current_screen != app::Screen::AddAccount) {
                state.current_screen = app::Screen::AddAccount;
                state.selected_account_id.clear();  // ensure the tab bar shows
            }

            widgets::draw_rail_nav(state);
            ImGui::SameLine(0, 0);

            ImGui::BeginChild("##content", ImVec2(0, 0), false, ImGuiWindowFlags_None);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, 10.0F));

            ImGui::Dummy(ImVec2(0, kContentPaddingY));
            ImGui::Indent(kContentPaddingX);
            ImGui::PushItemWidth(-kContentPaddingX);

            draw_screen(state);

            ImGui::Dummy(ImVec2(0, 24));

            ImGui::PopItemWidth();
            ImGui::Unindent(kContentPaddingX);
            ImGui::PopStyleVar();
            ImGui::EndChild();
        } else {
            ImGui::Dummy(ImVec2(0, 24));
            ImGui::Indent(24);
            screens::draw_unlock(state);
            ImGui::Unindent(24);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    state.toasts.render([&state](const std::string& account_id) {
        if (account_id.empty()) return;
        state.selected_account_id = account_id;
        state.current_screen = app::Screen::Accounts;
    });

    return keep_running;
}

}  // namespace sam::ui
