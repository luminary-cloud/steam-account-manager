#include "ui/screens/settings_screen.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <windows.h>

#include <imgui.h>

#include "app/app_paths.hpp"
#include "core/log.hpp"
#include "platform/window_affinity.hpp"
#include "ui/screens/settings_sections.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::screens {

namespace {

void draw_general_section(app::AppState& state) {
    separator_text("General");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Clipboard auto-clear (s)", &state.settings.clipboard_clear_seconds, 10, 120);
    hover_tooltip("Passwords and Steam Guard codes copied from the app are wiped from the "
                  "clipboard after this many seconds.");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Auto-lock (minutes)", &state.settings.auto_lock_minutes, 0, 240);
    hover_tooltip("Re-lock the vault after this many idle minutes. 0 disables auto-lock for the "
                  "current session.");
    ImGui::Checkbox("Check for updates on launch", &state.settings.check_updates_on_launch);
    hover_tooltip("On launch, checks GitHub for a newer release and shows an \"Update available\" "
                  "prompt if one exists. No account data is sent.");
}

void draw_appearance_section(app::AppState& state) {
    separator_text("Appearance");
    {
        int view_idx = static_cast<int>(state.settings.accounts_view);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Accounts view", &view_idx, "Grid\0List\0")) {
            state.settings.accounts_view = (view_idx == 1)
                ? app::AccountsViewMode::List
                : app::AccountsViewMode::Grid;
        }
        hover_tooltip("Grid: responsive card grid (the default). "
                      "List: two-pane layout with user-created groups on the left "
                      "and the selected account on the right.");
    }
    ImGui::Checkbox("Hide account notes", &state.settings.hide_notes);
    hover_tooltip("Hides account notes in grid and list views, including the selected "
                  "account's detail panel. Notes stay editable on the add/edit screen.");
}

void draw_privacy_section(app::AppState& state) {
    separator_text("Privacy");
    if (ImGui::Checkbox("Privacy mode - hide login names", &state.settings.privacy_mode)) {
        // Reset per-account reveals on either toggle direction.
        state.clear_session_secrets();
    }
    hover_tooltip("Replaces account login names with <hidden> everywhere they appear. Click a "
                  "redacted name to reveal that one account for the rest of the session; click "
                  "again to re-hide. Reveals are cleared when the vault locks.");

    if (ImGui::Checkbox("Hide from screen capture (streamproof)", &state.settings.streamproof)) {
        platform::set_capture_excluded(state.main_hwnd, state.settings.streamproof);
        state.save_settings();
    }
    hover_tooltip("Excludes this window from screen-capture software (OBS, Discord, Snipping "
                  "Tool) - they record a blank where the window is. It stays visible on your "
                  "monitor. Requires Windows 10 2004 or newer; some hardware capture cards may "
                  "still see it.");
}

void draw_account_info_section(app::AppState& state) {
    separator_text("Account info to display");
    if (ImGui::BeginTable("##info-grid", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); ImGui::Checkbox("VAC ban",            &state.settings.info.show_vac);
        ImGui::TableNextColumn(); ImGui::Checkbox("Game ban",           &state.settings.info.show_game_ban);
        ImGui::TableNextColumn(); ImGui::Checkbox("Community ban",      &state.settings.info.show_community_ban);

        ImGui::TableNextColumn(); ImGui::Checkbox("Trade ban",          &state.settings.info.show_trade_ban);
        ImGui::TableNextColumn(); ImGui::Checkbox("Steam level",        &state.settings.info.show_steam_level);
        ImGui::TableNextColumn(); ImGui::Checkbox("Owned games count",  &state.settings.info.show_owned_games);

        // Populated by the GCPD scraper (gcpd_enabled below); needs a fresh login.
        ImGui::TableNextColumn(); ImGui::Checkbox("Premier rating",     &state.settings.info.show_premier);
        ImGui::TableNextColumn(); ImGui::Checkbox("Wingman rank",       &state.settings.info.show_wingman);

        ImGui::TableNextColumn(); ImGui::Checkbox("Prime status",       &state.settings.info.show_prime);
        ImGui::TableNextColumn(); ImGui::Checkbox("VAC-Live indicator", &state.settings.info.show_vac_live);
        ImGui::TableNextColumn(); ImGui::Checkbox("Cooldown countdown", &state.settings.info.show_cooldown);
        ImGui::TableNextColumn(); ImGui::Checkbox("Weekly XP drop",     &state.settings.info.show_weekly_drop);
        ImGui::TableNextColumn(); ImGui::Checkbox("External funds",     &state.settings.info.show_external_funds);

        ImGui::EndTable();
    }
}

void draw_integration_section(app::AppState& state) {
    separator_text("Integration");
    std::array<char, 64> key_buf{};
    std::snprintf(key_buf.data(), key_buf.size(), "%s", state.settings.web_api_key.c_str());
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText("Steam Web API key", key_buf.data(), key_buf.size(),
                          ImGuiInputTextFlags_Password)) {
        state.settings.web_api_key = key_buf.data();
    }
    hover_tooltip("Get a key at steamcommunity.com/dev/apikey. Used by the public-data refresh "
                  "(level, owned games, ban status). Stored encrypted with the vault.");
    ImGui::Checkbox("Refresh on launch", &state.settings.refresh_on_launch);
    hover_tooltip("Re-query the Steam Web API for every account on startup. Off = no network "
                  "calls at launch.");
    ImGui::Checkbox("GCPD scraper", &state.settings.gcpd_enabled);
    hover_tooltip("Populates Premier rating, Wingman rank, Prime status, and competitive "
                  "cooldown by scraping the Game Coordinator Personal Data page. Requires a "
                  "valid steamLoginSecure cookie (use the Full Login wizard in Add Account).");
}

void draw_startup_section(app::AppState& state) {
    separator_text("Startup");

    int mode = static_cast<int>(state.settings.logon_action);
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("At Windows logon", &mode,
                     "Do nothing\0Refresh accounts in the background\0Open the app\0")) {
        const auto prev = state.settings.logon_action;
        state.settings.logon_action = static_cast<app::LogonAction>(mode);

        if (state.settings.logon_action == app::LogonAction::BackgroundRefresh) {
            // Background refresh needs the vault to auto-unlock: enable
            // refresh-on-launch and the DPAPI password cache alongside it.
            state.settings.refresh_on_launch = true;
            if (settings_detail::write_master_pw_cache(state))
                state.settings.remember_master_password = true;
            if (!state.settings.remember_master_password) {
                SAM_LOG_ERROR("startup: background refresh needs the master-password cache");
                state.settings.logon_action = prev;
            }
        }
        state.sync_logon_task();
        state.save_settings();
    }
    hover_tooltip("Do nothing: no logon task. Refresh accounts in the background: runs this app "
                  "hidden at logon with admin rights, refreshes every account, shows a Windows "
                  "notification for any new ban or cooldown, then exits. Open the app: launches "
                  "the full window at logon. The app is requireAdministrator, so this uses a "
                  "Scheduled Task (the Run key can't auto-start elevated apps).");

    if (state.settings.logon_action == app::LogonAction::OpenApp) {
        if (ImGui::Checkbox("Start minimized", &state.settings.start_minimized)) {
            state.sync_logon_task();  // the task's --minimized argument changes
            state.save_settings();
        }
        hover_tooltip("Opens minimized to the taskbar at logon. Manual launches always open "
                      "normally.");
    }

    if (state.settings.logon_action == app::LogonAction::BackgroundRefresh) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("Caches your master password via Windows DPAPI so the background run "
                           "can open the vault unattended. Anyone signed in as you on this PC can "
                           "then open the vault without the password.");
        ImGui::PopStyleColor();
    }
}

void draw_notifications_section(app::AppState& state) {
    separator_text("Notifications");
    ImGui::Checkbox("Detect bans and cooldown changes", &state.settings.notifications.enabled);
    hover_tooltip("When on, each refresh compares the new ban / cooldown state against the "
                  "previous snapshot and records a notification if anything changed. The "
                  "first refresh after adding an account just records the snapshot.");
    ImGui::BeginDisabled(!state.settings.notifications.enabled);
    if (ImGui::BeginTable("##notif-surface", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::Checkbox("Show badge on cards / rows",
                        &state.settings.notifications.surface_in_card);
        ImGui::TableNextColumn();
        ImGui::Checkbox("Show in-app toasts",
                        &state.settings.notifications.surface_toast);
        ImGui::TableNextColumn();
        ImGui::Checkbox("Show Windows notifications",
                        &state.settings.notifications.surface_windows_notification);
        hover_tooltip("Out-of-app tray balloon for new bans / cooldowns. Suppressed while this "
                      "window is focused; mainly for the background logon refresh.");
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Notify on");
    if (ImGui::BeginTable("##notif-kinds", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); ImGui::Checkbox("VAC ban",        &state.settings.notifications.on_new_vac_ban);
        ImGui::TableNextColumn(); ImGui::Checkbox("Game ban",       &state.settings.notifications.on_new_game_ban);
        ImGui::TableNextColumn(); ImGui::Checkbox("Community ban",  &state.settings.notifications.on_new_community_ban);
        ImGui::TableNextColumn(); ImGui::Checkbox("Trade ban",      &state.settings.notifications.on_new_trade_ban);
        ImGui::TableNextColumn(); ImGui::Checkbox("VAC-Live",       &state.settings.notifications.on_new_vac_live);
        ImGui::TableNextColumn(); ImGui::Checkbox("Ban removed",    &state.settings.notifications.on_ban_removed);
        ImGui::TableNextColumn(); ImGui::Checkbox("Cooldown started", &state.settings.notifications.on_cooldown_started);
        ImGui::TableNextColumn(); ImGui::Checkbox("Cooldown ended",   &state.settings.notifications.on_cooldown_ended);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Toast duration (s)",
                     &state.settings.notifications.toast_duration_seconds, 2, 30);
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Coalesce above N accounts",
                     &state.settings.notifications.coalesce_threshold, 2, 50);
    hover_tooltip("If more than this many accounts change in one refresh, a single summary "
                  "toast replaces the per-account toasts. Per-account badges still appear.");
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Keep history (days)",
                     &state.settings.notifications.retention_days, 1, 365);
    ImGui::EndDisabled();
}

void draw_list_view_section(app::AppState& state) {
    separator_text("List view");
    ImGui::Checkbox("Show cooldown marker on rows",
                    &state.settings.list_view.show_cooldown_marker);
    hover_tooltip("Adds an orange exclamation mark to the right of any list row whose "
                  "account is currently on a CS2 competitive cooldown.");
    ImGui::Checkbox("Show unread-event badge on rows",
                    &state.settings.list_view.show_unread_badge);
    hover_tooltip("Adds a red exclamation mark to the right of any list row that has "
                  "un-acknowledged ban or cooldown notifications.");
    ImGui::Checkbox("Show weekly-drop marker on rows",
                    &state.settings.list_view.show_weekly_drop_marker);
    hover_tooltip("Adds a green checkmark to the right of any list row whose account is "
                  "marked as having claimed its weekly CS2 XP drop.");
    ImGui::Checkbox("Hide account name in list",
                    &state.settings.list_view.hide_account_name);
    hover_tooltip("Hides the Steam login/account name from account-list rows. The persona name "
                  "still shows, and the selected account's detail panel is unaffected.");
}

void draw_vault_section(app::AppState& state) {
    separator_text("Vault");
    {
        const bool prev = state.settings.remember_master_password;
        if (ImGui::Checkbox("Skip master-password prompt on launch (DPAPI)",
                            &state.settings.remember_master_password)) {
            if (state.settings.remember_master_password && !prev) {
                // Revert the toggle if the cache write fails.
                if (!settings_detail::write_master_pw_cache(state)) {
                    state.settings.remember_master_password = false;
                }
            } else if (!state.settings.remember_master_password && prev) {
                // Delete the cache so the next launch prompts.
                std::error_code ec;
                std::filesystem::remove(app::master_pw_cache_path(), ec);
                SAM_LOG_INFO("auto-unlock: DPAPI cache removed");
            }
            state.save_settings();
        }
    }
    hover_tooltip("Caches your master password under your Windows account via DPAPI so the "
                  "vault opens automatically next launch. Anyone signed in as you on this "
                  "machine can open the vault without typing the password. Disabling this "
                  "option deletes the cached password.");
}

}  // namespace

// Gap above the pinned Save settings footer row.
constexpr float kFooterGap = 8.0F;

void draw_settings(app::AppState& state) {
    ImGui::TextUnformatted("Settings");
    ImGui::Spacing();

    // Pin the footer: the body scrolls in a child that reserves room for everything
    // drawn after it, so the parent doesn't grow a second scrollbar. Trailing content
    // is the gap + button row plus three item-spacings and main_window's Dummy(0,24).
    constexpr float kButtonRowHeight = 26.0F;  // action_button height
    constexpr float kTrailingDummy   = 24.0F;  // main_window's Dummy after draw_screen
    const float footer_reserved = ImGui::GetStyle().ItemSpacing.y * 3.0F +
                                  kFooterGap + kButtonRowHeight + kTrailingDummy;

    ImGui::BeginChild("##settings-body", ImVec2(0, -footer_reserved),
                      ImGuiChildFlags_NavFlattened);
    // PushItemWidth doesn't carry across the child boundary; re-apply so right edges match.
    ImGui::PushItemWidth(-kContentPaddingX);

    draw_general_section(state);

    ImGui::Spacing();
    draw_appearance_section(state);

    ImGui::Spacing();
    draw_privacy_section(state);

    ImGui::Spacing();
    draw_account_info_section(state);

    ImGui::Spacing();
    draw_integration_section(state);

    ImGui::Spacing();
    settings_detail::draw_proxy_section(state);

    ImGui::Spacing();
    draw_startup_section(state);

    ImGui::Spacing();
    draw_notifications_section(state);

    ImGui::Spacing();
    settings_detail::draw_authenticator_section(state);

    ImGui::Spacing();
    draw_list_view_section(state);

    ImGui::Spacing();
    settings_detail::draw_confirmations_section(state);

    ImGui::Spacing();
    draw_vault_section(state);

    ImGui::Spacing();
    settings_detail::draw_cs2_config_section(state);

    ImGui::Spacing();
    settings_detail::draw_gamesense_section(state);

    ImGui::Spacing();
    settings_detail::draw_storage_section(state);

    ImGui::PopItemWidth();
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0.0F, kFooterGap));
    if (action_button("Save settings", ImVec2(160, 0))) {
        state.save_settings();
        ImGui::OpenPopup("Settings saved");
    }
    hover_tooltip("Persist settings to the config directory.");

    if (begin_styled_modal("Settings saved")) {
        ImGui::TextWrapped("Settings saved.");
        ImGui::Spacing();
        if (action_button("OK", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }
}

}  // namespace sam::ui::screens
