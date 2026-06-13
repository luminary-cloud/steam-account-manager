#include "ui/main_window.hpp"

#include <mutex>
#include <string>

#include <imgui.h>

#include "core/version.hpp"
#include "ui/screens/accounts_screen.hpp"
#include "ui/screens/add_account_screen.hpp"
#include "ui/screens/confirmations_screen.hpp"
#include "ui/screens/sda_screen.hpp"
#include "ui/screens/settings_screen.hpp"
#include "ui/screens/trade_offers_screen.hpp"
#include "ui/screens/unlock_screen.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/rail_nav.hpp"
#include "ui/widgets/title_bar.hpp"

namespace sam::ui {

namespace {

// kContentPaddingX lives in ui/util.hpp so the separator helpers can share it.
constexpr float kContentPaddingY = 20.0F;

void draw_screen(app::AppState& state) {
    switch (state.current_screen) {
        case app::Screen::Unlock:        screens::draw_unlock(state);        break;
        case app::Screen::Accounts:      screens::draw_accounts(state);      break;
        case app::Screen::Authenticator: screens::draw_sda(state);           break;
        case app::Screen::Confirmations: screens::draw_confirmations(state); break;
        case app::Screen::TradeOffers:   screens::draw_trade_offers(state);  break;
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
        widgets::draw_title_bar(state);
        // Flush under the bar, else an ItemSpacing gap shows a strip of root background.
        ImGui::SetCursorPosY(widgets::kTitleBarHeight);

        if (state.unlocked) {
            // A background refresh flagged an account for re-login: jump to Add Account
            // so the Full Login wizard picks up the prefilled username (and clears it).
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

    // Only once unlocked, so the modal never steals focus from the Unlock password field.
    if (state.unlocked) {
        {
            std::lock_guard lk(state.update_mutex);
            if (state.update_result && !state.update_modal_dismissed_this_session &&
                state.update_result->latest_tag != state.settings.version_check_skip_until) {
                ImGui::OpenPopup("Update available");
            }
        }
        if (begin_styled_modal("Update available", 500.0F)) {
            std::string latest;
            {
                std::lock_guard lk(state.update_mutex);
                if (state.update_result) latest = state.update_result->latest_tag;
            }
            ImGui::TextWrapped("Steam Account Manager %s is available. You're on v%s.",
                               latest.c_str(), std::string(sam::kVersion).c_str());
            ImGui::Spacing();
            const ImVec2 btn{146, 28};
            if (ImGui::Button("Open releases", btn)) {
                open_url(std::string(sam::kRepoUrl) + "/releases");
                state.update_modal_dismissed_this_session = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Skip this version", btn)) {
                state.settings.version_check_skip_until = latest;
                state.save_settings();
                state.update_modal_dismissed_this_session = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Remind me later", btn)) {
                state.update_modal_dismissed_this_session = true;
                ImGui::CloseCurrentPopup();
            }
            end_styled_modal();
        }
    }

    return keep_running;
}

}  // namespace sam::ui
