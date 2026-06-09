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
#include "core/sda/totp.hpp"
#include "platform/clipboard.hpp"
#include "ui/util.hpp"

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

    // Sign the default browser in to this account and open its profile in a
    // private window. Needs a web-capable refresh token (or a saved password to
    // mint one), so it's unavailable for token-only (NFA) accounts.
    const bool can_browser = has_sid && !a.is_nfa &&
                             (!a.refresh_token.empty() || !a.password.empty());
    if (ImGui::MenuItem("Open in browser (signed in)", nullptr, false, can_browser)) {
        state.open_account_in_browser(a);
    }
    if (!can_browser && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const char* why = a.is_nfa
            ? "Token-only (NFA) accounts can't open a web session"
            : (a.steam_id_64 == 0 ? "No SteamID yet - refresh the account first"
                                  : "Needs a saved password to sign in");
        set_tooltip("%s", why);
    }

    ImGui::Separator();

    const auto cs2_mode = state.settings.cs2_video.mode;
    if (cs2_mode != app::CS2ConfigMode::None) {
        const char* cfg_label = "Add video config";
        bool cfg_ready = false;
        if (cs2_mode == app::CS2ConfigMode::VideoTxt) {
            cfg_ready = std::filesystem::exists(app::cs2_video_template_path());
        } else {  // Folder730
            cfg_label = "Apply 730 folder";
            cfg_ready = std::filesystem::is_directory(app::cs2_730_template_dir());
        }
        if (ImGui::MenuItem(cfg_label, nullptr, false, has_sid && cfg_ready)) {
            state.apply_cs2_video_config(a);
        }
    }

    if (ImGui::MenuItem("Change username", nullptr, false, has_sid)) {
        state.selected_account_id = a.id;
        state.persona_change_requested = true;
    }

    // NFA accounts can't scrape their CS2 cooldown from GCPD, so let the user set
    // it by hand from the known cooldown tiers.
    if (a.is_nfa) {
        ImGui::Separator();
        if (ImGui::BeginMenu("Competitive cooldown")) {
            struct CooldownOption { const char* label; long long seconds; };
            static constexpr CooldownOption kOptions[] = {
                {"20 hours", 20LL * 3600},
                {"7 days",   7LL * 86400},
                {"31 days",  31LL * 86400},
                {"181 days", 181LL * 86400},
            };
            auto set_cooldown = [&](long long expires) {
                if (auto* acc = state.find_account(a.id)) {
                    acc->cs2.cooldown_expires_unix = expires;
                    acc->cs2.cooldown_reason = expires > 0 ? "Competitive cooldown" : "";
                    // Mirror the snapshot so the next refresh diff doesn't fire a
                    // cooldown notification for a value we set ourselves.
                    acc->prev_snapshot.cooldown_expires_unix = expires;
                    state.vault_dirty = true;
                    state.save_vault_if_dirty();
                }
            };
            const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (const auto& o : kOptions) {
                if (ImGui::MenuItem(o.label)) set_cooldown(now + o.seconds);
            }
            if (ImGui::MenuItem("Permanent")) set_cooldown(sam::core::kCooldownNever);
            if (a.cs2.cooldown_expires_unix > 0) {
                ImGui::Separator();
                if (ImGui::MenuItem("Clear")) set_cooldown(0);
            }
            ImGui::EndMenu();
        }
    }

    // Weekly XP drop is set purely by hand (there's no scrape source), so offer it
    // for every account. Marking it claimed stores the next weekly reset time; the
    // marker auto-clears once that moment passes.
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
        if (ImGui::MenuItem("Mark claimed", nullptr, false, !claimed)) {
            set_drop(next_weekly_reset(now));
        }
        if (a.cs2.weekly_drop_reset_unix != 0) {
            if (ImGui::MenuItem("Clear")) set_drop(0);
        }
        ImGui::EndMenu();
    }
}

}  // namespace sam::ui::widgets
