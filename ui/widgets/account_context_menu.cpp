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

    // Inapplicable items are hidden, not greyed out.
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

    // Copies the cached link instantly; on a cache miss/expiry copy_trade_link
    // scrapes it in the background and copies when ready. Hidden only for token-
    // only (NFA) accounts with nothing cached, which can't scrape one.
    const bool has_trade_link = a.trade_url.has_value() && !a.trade_url->empty();
    const bool can_fetch_trade = has_sid && !a.is_nfa &&
                                 (!a.refresh_token.empty() || !a.password.empty());
    if ((has_trade_link || can_fetch_trade) && ImGui::MenuItem("Copy trade link")) {
        state.copy_trade_link(a.id);
    }

    // Needs a web-capable refresh token (or a saved password to mint one), so
    // it's hidden for token-only (NFA) accounts.
    const bool can_browser = has_sid && !a.is_nfa &&
                             (!a.refresh_token.empty() || !a.password.empty());
    if (can_browser && ImGui::MenuItem("Open in browser (signed in)")) {
        state.open_account_in_browser(a);
    }

    // Video config + Change username both need a SteamID.
    if (has_sid) {
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
            if (cfg_ready && ImGui::MenuItem(cfg_label)) {
                state.apply_cs2_video_config(a);
            }
        }

        if (ImGui::MenuItem("Change username")) {
            state.selected_account_id = a.id;
            state.persona_change_requested = true;
        }
    }

    // NFA accounts can't scrape their cooldown from GCPD, so allow setting it by
    // hand from the known tiers.
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
                    // notification for a value we set ourselves.
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

    // No scrape source, so this is hand-set for every account. Marking claimed
    // stores the next weekly reset; the marker auto-clears once it passes.
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
}

}  // namespace sam::ui::widgets
