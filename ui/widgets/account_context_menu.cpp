#include "ui/widgets/account_context_menu.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <imgui.h>

#include "app/app_paths.hpp"
#include "core/cs2/friend_code.hpp"
#include "core/sda/totp.hpp"
#include "platform/clipboard.hpp"

namespace sam::ui::widgets {

void draw_account_context_menu(app::AppState& state, const core::Account& a) {
    const auto secs = std::chrono::seconds(state.settings.clipboard_clear_seconds);

    if (ImGui::MenuItem("Copy login", nullptr, false, !a.login.empty())) {
        platform::clipboard::set_text(a.login);
    }

    const bool has_pw = !a.password.empty();
    if (ImGui::MenuItem("Copy password", nullptr, false, has_pw)) {
        platform::clipboard::set_text_with_auto_clear(
            std::string_view(a.password.c_str(), a.password.size()), secs);
    }

    const bool has_2fa = a.sda.has_value() && !a.sda->shared_secret.empty();
    if (ImGui::MenuItem("Copy 2FA code", nullptr, false, has_2fa)) {
        const std::string code = sda::generate_code_now(a.sda->shared_secret);
        if (!code.empty()) {
            platform::clipboard::set_text_with_auto_clear(code, secs);
        }
    }

    const bool has_sid = a.steam_id_64 != 0;
    if (ImGui::MenuItem("Copy SteamID64", nullptr, false, has_sid)) {
        char sid[24];
        std::snprintf(sid, sizeof(sid), "%llu",
                      static_cast<unsigned long long>(a.steam_id_64));
        platform::clipboard::set_text(sid);
    }

    if (ImGui::MenuItem("Copy profile URL", nullptr, false, has_sid)) {
        char url[64];
        std::snprintf(url, sizeof(url), "https://steamcommunity.com/profiles/%llu",
                      static_cast<unsigned long long>(a.steam_id_64));
        platform::clipboard::set_text(url);
    }

    if (ImGui::MenuItem("Copy CS2 friend code", nullptr, false, has_sid)) {
        const std::string code = cs2::friend_code(a.steam_id_64);
        if (!code.empty()) platform::clipboard::set_text(code);
    }

    ImGui::Separator();

    const bool has_template =
        std::filesystem::exists(app::cs2_video_template_path());
    if (ImGui::MenuItem("Add video config", nullptr, false, has_sid && has_template)) {
        state.apply_cs2_video_config(a);
    }

    if (ImGui::MenuItem("Change username", nullptr, false, has_sid)) {
        state.selected_account_id = a.id;
        state.persona_change_requested = true;
    }
}

}  // namespace sam::ui::widgets
