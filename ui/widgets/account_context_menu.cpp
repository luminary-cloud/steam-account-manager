#include "ui/widgets/account_context_menu.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <imgui.h>

#include "app/app_paths.hpp"
#include "core/account_store/account.hpp"
#include "core/cs2/friend_code.hpp"
#include "core/hwid/hwid_gen.hpp"
#include "core/sda/totp.hpp"
#include "platform/clipboard.hpp"
#include "ui/util.hpp"

namespace sam::ui::widgets {

void draw_account_context_menu(app::AppState& state, const core::Account& a) {
    const auto secs = std::chrono::seconds(state.settings.clipboard_clear_seconds);

    if (!a.login.empty() && ImGui::MenuItem("Copy login")) {
        platform::clipboard::set_text(a.login);
    }

    if (!a.password.empty() && ImGui::MenuItem("Copy password")) {
        platform::clipboard::set_text_with_auto_clear(
            std::string_view(a.password.c_str(), a.password.size()), secs);
    }

    const bool has_2fa = a.sda.has_value() && !a.sda->shared_secret.empty();
    if (has_2fa && ImGui::MenuItem("Copy 2FA code")) {
        const std::string code = sda::generate_code_now(a.sda->shared_secret);
        if (!code.empty()) {
            platform::clipboard::set_text_with_auto_clear(code, secs);
        }
    }

    const bool has_sid = a.steam_id_64 != 0;
    if (has_sid && ImGui::MenuItem("Copy SteamID64")) {
        char sid[24];
        std::snprintf(sid, sizeof(sid), "%llu",
                      static_cast<unsigned long long>(a.steam_id_64));
        platform::clipboard::set_text(sid);
    }

    if (has_sid && ImGui::MenuItem("Copy profile URL")) {
        char url[64];
        std::snprintf(url, sizeof(url), "https://steamcommunity.com/profiles/%llu",
                      static_cast<unsigned long long>(a.steam_id_64));
        platform::clipboard::set_text(url);
    }

    if (has_sid && ImGui::MenuItem("Copy CS2 friend code")) {
        const std::string code = cs2::friend_code(a.steam_id_64);
        if (!code.empty()) platform::clipboard::set_text(code);
    }

    const bool has_trade_link = a.trade_url.has_value() && !a.trade_url->empty();
    const bool can_fetch_trade = has_sid &&
                                 (!a.refresh_token.empty() || !a.password.empty());
    if ((has_trade_link || can_fetch_trade) && ImGui::MenuItem("Copy trade link")) {
        state.copy_trade_link(a.id);
    }

    const bool can_browser = has_sid && !a.is_nfa &&
                             (!a.refresh_token.empty() || !a.password.empty());
    if (can_browser && ImGui::MenuItem("Open in browser (signed in)")) {
        state.open_account_in_browser(a);
    }

    if (has_sid) {
        ImGui::Separator();

        const auto cs2_mode = state.settings.cs2_video.mode;
        if (cs2_mode != app::CS2ConfigMode::None) {
            const char* cfg_label = "Add video config";
            bool cfg_ready = false;
            switch (cs2_mode) {
                case app::CS2ConfigMode::VideoTxt:
                    cfg_ready = std::filesystem::exists(app::cs2_video_template_path());
                    break;
                case app::CS2ConfigMode::Folder730:
                    cfg_label = "Apply 730 folder";
                    cfg_ready = std::filesystem::is_directory(app::cs2_730_template_dir());
                    break;
                case app::CS2ConfigMode::UserdataFolder:
                    cfg_label = "Apply game folders";
                    cfg_ready = std::filesystem::is_directory(app::userdata_template_dir());
                    break;
                case app::CS2ConfigMode::None:
                    break;
            }
            if (cfg_ready && ImGui::MenuItem(cfg_label)) {
                state.apply_cs2_video_config(a);
            }
        }

        if (ImGui::MenuItem("Change username")) {
            state.selected_account_id = a.id;
            state.persona_change_requested = true;
        }
    }

    if (ImGui::MenuItem("Edit notes")) {
        state.selected_account_id = a.id;
        state.notes_edit_requested = true;
    }

    if (a.is_nfa && a.steam_id_64 != 0 && ImGui::MenuItem("Validate token / refresh GC")) {
        state.queue_gc_validate(a.id);
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Weekly XP drop")) {
        auto set_drop = [&](std::int64_t reset_unix) {
            if (auto* acc = state.find_account(a.id)) {
                acc->cs2.weekly_drop_reset_unix = reset_unix;
                state.vault_dirty = true;
                state.save_vault_if_dirty();
            }
        };
        const std::int64_t now = now_seconds();
        const bool claimed = a.cs2.weekly_drop_reset_unix > now;
        if (!claimed && ImGui::MenuItem("Mark claimed")) {
            set_drop(next_weekly_reset(now));
        }
        if (claimed && ImGui::MenuItem("Clear")) {
            set_drop(0);
        }
        ImGui::EndMenu();
    }

    if (!state.settings.safe_mode && ImGui::BeginMenu("HWID spoofer")) {
        const bool enabled = a.hwid.has_value();
        const bool always  = state.settings.hwid.always_spoof;

        if (always && !a.hwid_excluded && ImGui::MenuItem("Exclude from spoof")) {
            if (auto* acc = state.find_account(a.id)) {
                acc->hwid_excluded = true;
                state.vault_dirty = true;
                state.save_vault_if_dirty();
            }
        }
        if (always && a.hwid_excluded && ImGui::MenuItem("Include in spoof")) {
            if (auto* acc = state.find_account(a.id)) {
                acc->hwid_excluded = false;
                state.vault_dirty = true;
                state.save_vault_if_dirty();
            }
        }
        if (!always && !enabled && ImGui::MenuItem("Enable")) {
            if (auto* acc = state.find_account(a.id)) {
                acc->hwid = core::hwid::generate_profile();
                state.vault_dirty = true;
                state.save_vault_if_dirty();
            }
        }
        if (enabled && ImGui::MenuItem("Regenerate")) {
            if (auto* acc = state.find_account(a.id)) {
                acc->hwid = core::hwid::generate_profile();
                state.vault_dirty = true;
                state.save_vault_if_dirty();
            }
        }
        if (!always && enabled && ImGui::MenuItem("Disable")) {
            if (auto* acc = state.find_account(a.id)) {
                acc->hwid = std::nullopt;
                state.vault_dirty = true;
                state.save_vault_if_dirty();
            }
        }
        ImGui::EndMenu();
    }
}

}  // namespace sam::ui::widgets
