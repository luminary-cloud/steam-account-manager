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
#include "core/cs2_config/workshop_block.hpp"
#include "core/log.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/window_affinity.hpp"
#include "ui/fonts.hpp"
#include "ui/screens/settings_sections.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::screens {

namespace {

void global_badge() {
    ImGui::SameLine();
    ImGui::TextDisabled("(global)");
    hover_tooltip("Applies to every vault, not just this one.");
}

void draw_clipboard_lock_section(app::AppState& state) {
    separator_text("Clipboard & auto-lock");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Clipboard auto-clear (s)", &state.settings.clipboard_clear_seconds, 10, 120);
    hover_tooltip("Wipes copied passwords and codes from the clipboard after this long.");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Auto-lock (minutes)", &state.settings.auto_lock_minutes, 0, 240);
    hover_tooltip("Re-locks the vault after this long idle. 0 disables it.");
}

void draw_updates_section(app::AppState& state) {
    separator_text("Updates");
    ImGui::Checkbox("Check for updates on launch", &state.settings.check_updates_on_launch);
    hover_tooltip("Checks GitHub for a newer release at startup. No account data is sent.");
    global_badge();
}

void draw_appearance_section(app::AppState& state) {
    {
        int view_idx = static_cast<int>(state.settings.accounts_view);
        ImGui::SetNextItemWidth(200);
        if (styled_combo("Accounts view", &view_idx, "Grid\0List\0")) {
            state.settings.accounts_view = (view_idx == 1)
                ? app::AccountsViewMode::List
                : app::AccountsViewMode::Grid;
        }
        hover_tooltip("Grid: card grid. List: two-pane layout with groups on the left.");
    }
    ImGui::Checkbox("Hide account notes", &state.settings.hide_notes);
    hover_tooltip("Hides notes everywhere. They stay editable on the add/edit screen.");
}

void draw_safe_mode_section(app::AppState& state) {
    separator_text("Safe mode");

    bool on = state.settings.safe_mode;
    if (ImGui::Checkbox("Safe mode", &on)) {
        if (on) {
            state.settings.safe_mode = true;
            state.save_settings();
        } else {

            ImGui::OpenPopup("Disable safe mode?");
        }
    }
    hover_tooltip("Hides and disables the HWID spoofer, the external CS2 loaders and the "
                  "tracer cleaner. Nothing is erased; turning it off restores everything. "
                  "Per-vault.");

    if (state.settings.safe_mode) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("On. No launch injects the HWID spoofer or runs an external "
                           "loader, and the tracer cleaner never runs, whatever they are "
                           "set to.");
        ImGui::PopStyleColor();
    }

    if (begin_styled_modal("Disable safe mode?")) {
        ImGui::TextWrapped("This brings back the HWID spoofer, the external CS2 loaders "
                           "(gamesense, luminary) and the tracer cleaner, along with every "
                           "account's stored HWID profile and launch method. Launching an "
                           "account will inject and run them again, and any cleaner trigger "
                           "you armed will start firing.");
        ImGui::Spacing();
        if (action_button("Cancel", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (action_button("Disable safe mode", ImVec2(160, 0))) {
            state.settings.safe_mode = false;
            state.save_settings();
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }
}

void draw_privacy_section(app::AppState& state) {
    separator_text("Privacy");
    if (ImGui::Checkbox("Privacy mode - hide login names", &state.settings.privacy_mode)) {

        state.clear_session_secrets();
    }
    hover_tooltip("Replaces login names with <hidden>. Click one to reveal it until the "
                  "vault locks.");

    if (ImGui::Checkbox("Hide from screen capture (streamproof)", &state.settings.streamproof)) {
        platform::set_capture_excluded(state.main_hwnd, state.settings.streamproof);
        state.save_settings();
    }
    hover_tooltip("Capture software (OBS, Discord) records a blank where the window is. "
                  "Still visible on your monitor. Needs Windows 10 2004 or newer.");
}

void draw_steam_login_section(app::AppState& state) {
    separator_text("Steam login");

    {
        int method = static_cast<int>(state.settings.sign_in_method);
        ImGui::SetNextItemWidth(200);
        if (styled_combo("Sign-in method", &method,
                         "Sign-in driver\0Token injection\0")) {
            state.settings.sign_in_method = static_cast<app::SignInMethod>(method);
            state.save_settings();
        }
        hover_tooltip("Driver types into Steam's login window. Token injection skips the "
                      "window, but needs a stored password to mint the token once.");
    }

    if (ImGui::Checkbox("Disable Steam Cloud on login",
                        &state.settings.disable_cloud_on_login)) {
        state.save_settings();
    }
    hover_tooltip("Turns Steam Cloud off for every account you launch. This is an "
                  "account-wide Steam setting, and it is not put back.");

    if (ImGui::Checkbox("Disable new-release news on login",
                        &state.settings.disable_news_on_login)) {
        state.save_settings();
    }
    hover_tooltip("Turns off new-release notifications for every account you launch. "
                  "Not put back afterwards.");

    if (ImGui::Checkbox("Sign in as Invisible", &state.settings.login_invisible)) {
        state.save_settings();
    }
    hover_tooltip("Sets friends status to Invisible before sign-in, so switching never "
                  "flashes an account online. Not put back afterwards.");

    if (ImGui::Checkbox("Disable Remote Play on login",
                        &state.settings.disable_remote_play_on_login)) {
        state.save_settings();
    }
    hover_tooltip("Turns off Remote Play for every account you launch. Not put back "
                  "afterwards.");

    if (ImGui::Checkbox("Disable CS2 workshop map downloads on login",
                        &state.settings.disable_workshop_on_login)) {
        state.save_settings();
    }
    hover_tooltip("Stops subscribed workshop maps downloading, so CS2 starts straight "
                  "away. Subscriptions are kept and restored on the next launch.");

    if (ImGui::Button("Re-enable Steam workshop downloads")) {
        const auto r = cs2_config::clear_all_workshop_blocks();
        SAM_LOG_INFO("settings: clear workshop blocks -> ok={} ({})", r.ok, r.message);
    }
    hover_tooltip("Restores every account's workshop subscriptions on this PC.");

    {
        const bool token_mode =
            state.settings.sign_in_method == app::SignInMethod::TokenInject;
        ImGui::BeginDisabled(token_mode);
        if (ImGui::Checkbox("Remember password on login",
                            &state.settings.remember_password_on_login)) {
            state.save_settings();
        }
        hover_tooltip("Ticks Steam's \"Remember me\" box so the session is kept. "
                      "Ignored by token injection, which always needs one.");
        ImGui::EndDisabled();
    }
}

void draw_cs2_gc_section(app::AppState& state) {
    separator_text("CS2 Game Coordinator");
    ImGui::Checkbox("Enable CS2 tab", &state.settings.cs2_gc.enabled);
    hover_tooltip("Shows the CS2 tab and allows a live Game Coordinator connection for "
                  "inventory and weekly drops.");
    ImGui::BeginDisabled(!state.settings.cs2_gc.enabled);
    ImGui::Checkbox("Show weekly drop card", &state.settings.cs2_gc.show_weekly_drop);
    ImGui::Checkbox("Show weekly mission card", &state.settings.cs2_gc.show_weekly_mission);
    ImGui::Checkbox("Show inventory card", &state.settings.cs2_gc.show_inventory);
    ImGui::Checkbox("Show storage units card", &state.settings.cs2_gc.show_storage_units);
    ImGui::Checkbox("Mark weekly drop claimed automatically",
                    &state.settings.cs2_gc.auto_mark_claimed);
    hover_tooltip("Keeps the weekly-drop marker in step with the GC while connected. "
                  "Off leaves it to the manual \"Mark claimed\" action.");
    ImGui::Checkbox("Auto-refresh on startup",
                    &state.settings.cs2_gc.auto_pull_on_startup);
    hover_tooltip("Runs Refresh all and Refresh GC at startup, signing in where needed. "
                  "Skips anything still cached. External funds are never fetched.");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("GC cache duration (hours)", &state.settings.cs2_gc.cache_hours, 1, 24);
    hover_tooltip("How long GC data stays cached before auto-pull refreshes it.");
    ImGui::EndDisabled();
}

void draw_account_info_section(app::AppState& state) {
    separator_text("Account info to display");

    if (ImGui::BeginTable("##info-grid", 3, ImGuiTableFlags_SizingStretchSame)) {
        const char* kWebApi = "Needs a Steam Web API key, set under Network & Data.";
        const char* kGcpd   = "Needs the GCPD scraper, enabled under Network & Data.";
        const char* kGc     = "Needs the CS2 Game Coordinator, enabled under CS2.";

        ImGui::TableNextColumn(); ImGui::Checkbox("VAC ban",            &state.settings.info.show_vac);
        hover_tooltip(kWebApi);
        ImGui::TableNextColumn(); ImGui::Checkbox("Game ban",           &state.settings.info.show_game_ban);
        hover_tooltip(kWebApi);
        ImGui::TableNextColumn(); ImGui::Checkbox("Community ban",      &state.settings.info.show_community_ban);
        hover_tooltip(kWebApi);

        ImGui::TableNextColumn(); ImGui::Checkbox("Trade ban",          &state.settings.info.show_trade_ban);
        hover_tooltip(kWebApi);
        ImGui::TableNextColumn(); ImGui::Checkbox("Steam level",        &state.settings.info.show_steam_level);
        hover_tooltip(kWebApi);
        ImGui::TableNextColumn(); ImGui::Checkbox("Owned games count",  &state.settings.info.show_owned_games);
        hover_tooltip(kWebApi);
        ImGui::TableNextColumn(); ImGui::Checkbox("Steam ID",           &state.settings.info.show_steam_id);

        ImGui::TableNextColumn(); ImGui::Checkbox("Premier rating",     &state.settings.info.show_premier);
        hover_tooltip(kGcpd);
        ImGui::TableNextColumn(); ImGui::Checkbox("Wingman rank",       &state.settings.info.show_wingman);
        hover_tooltip(kGcpd);

        ImGui::TableNextColumn(); ImGui::Checkbox("Prime status",       &state.settings.info.show_prime);
        hover_tooltip("Inferred from CS2 level and XP, so it needs the GCPD scraper or the GC.");
        ImGui::TableNextColumn(); ImGui::Checkbox("VAC-Live indicator", &state.settings.info.show_vac_live);
        hover_tooltip("VAC ban seen by the CS2 Game Coordinator, usually before the Web API.");
        ImGui::TableNextColumn(); ImGui::Checkbox("Cooldown countdown", &state.settings.info.show_cooldown);
        hover_tooltip(kGc);
        ImGui::TableNextColumn(); ImGui::Checkbox("Weekly XP drop",     &state.settings.info.show_weekly_drop);
        hover_tooltip(kGc);
        ImGui::TableNextColumn(); ImGui::Checkbox("External funds",     &state.settings.info.show_external_funds);
        hover_tooltip("Filled by Refresh spent on the accounts toolbar, which signs in to "
                      "each account.");

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
    ImGui::SameLine();
    if (action_button("Get key")) {
        open_url("https://steamcommunity.com/dev/apikey");
    }
    hover_tooltip("Needed for bans, level and owned games. Stored encrypted with the vault.");
    ImGui::Checkbox("Refresh on launch", &state.settings.refresh_on_launch);
    hover_tooltip("Re-queries the Steam Web API for every account at startup.");
    ImGui::Checkbox("GCPD scraper", &state.settings.gcpd_enabled);
    hover_tooltip("Scrapes Premier rating, Wingman rank, Prime and cooldown. "
                  "Needs a full login (Add Account > Full Login).");

    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Steam data cache (hours)", &state.settings.steam_cache_hours, 1, 24);
    hover_tooltip("Batch refreshes skip accounts newer than this. A manual single-account "
                  "Refresh always forces.");

    separator_text("Auto-refresh");
    ImGui::Checkbox("Auto-refresh while open", &state.settings.auto_refresh_enabled);
    hover_tooltip("Refreshes accounts whose cache has expired, on a timer. Never runs "
                  "the funds or GCPD scrapes.");
    ImGui::BeginDisabled(!state.settings.auto_refresh_enabled);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Refresh interval (minutes)", &state.settings.auto_refresh_minutes, 10, 720);
    ImGui::EndDisabled();
}

void restart_elevated(app::AppState& state) {
    if (platform::process::relaunch_elevated(L"--switch")) {
        PostMessageW(state.main_hwnd, WM_CLOSE, 0, 0);
    }
}

void draw_admin_section(app::AppState& state) {
    separator_text("Administrator");

    if (ImGui::Checkbox("Run as administrator", &state.settings.run_as_admin)) {
        state.save_settings();
        if (state.settings.run_as_admin && !state.is_elevated) {
            ImGui::OpenPopup("Restart as administrator?");
        } else if (!state.settings.run_as_admin && state.is_elevated) {
            ImGui::OpenPopup("Close to drop administrator?");
        }
    }
    hover_tooltip("Shows a UAC prompt at every launch. Only the logon task below and an "
                  "elevated Steam need it; everything else works either way.");
    global_badge();

    if (state.settings.run_as_admin && !state.is_elevated) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("Not running as administrator this session.");
        ImGui::PopStyleColor();
        if (action_button("Restart as administrator", ImVec2(210, 0))) restart_elevated(state);
    }

    if (begin_styled_modal("Restart as administrator?")) {
        ImGui::TextWrapped("A process can't gain administrator rights while it's running, so "
                           "this takes effect on the next launch. Restart now?");
        ImGui::Spacing();
        if (action_button("Later", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (action_button("Restart now", ImVec2(140, 0))) {
            ImGui::CloseCurrentPopup();
            restart_elevated(state);
        }
        end_styled_modal();
    }

    if (begin_styled_modal("Close to drop administrator?")) {
        ImGui::TextWrapped("This session stays elevated. An elevated process can't restart "
                           "itself as a normal one. Close the app and open it again, and it "
                           "will start without the UAC prompt.");
        ImGui::Spacing();
        if (action_button("Keep it open", ImVec2(140, 0))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (action_button("Close now", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
            PostMessageW(state.main_hwnd, WM_CLOSE, 0, 0);
        }
        end_styled_modal();
    }
}

bool s_logon_task_ok = true;

void draw_startup_section(app::AppState& state) {
    separator_text("Startup");

    ImGui::BeginDisabled(!state.is_elevated);

    int mode = static_cast<int>(state.settings.logon_action);
    ImGui::SetNextItemWidth(260);
    if (styled_combo("At Windows logon", &mode,
                     "Do nothing\0Refresh accounts in the background\0Open the app\0")) {
        const auto prev = state.settings.logon_action;
        state.settings.logon_action = static_cast<app::LogonAction>(mode);

        if (state.settings.logon_action == app::LogonAction::BackgroundRefresh) {

            state.settings.refresh_on_launch = true;
            if (settings_detail::write_master_pw_cache(state))
                state.settings.remember_master_password = true;
            if (!state.settings.remember_master_password) {
                SAM_LOG_ERROR("startup: background refresh needs the master-password cache");
                state.settings.logon_action = prev;
            } else if (state.vault_registry.vaults.size() > 1) {

                app::set_auto_open(state.vault_registry, platform::active_vault_id());
            }
        }
        s_logon_task_ok = state.sync_logon_task();
        state.save_settings();
    }
    hover_tooltip("Background refresh runs hidden at logon, notifies on new bans or "
                  "cooldowns, then exits. Open the app launches the full window.");
    global_badge();

    if (state.settings.logon_action == app::LogonAction::OpenApp) {
        if (ImGui::Checkbox("Start minimized", &state.settings.start_minimized)) {

            s_logon_task_ok = state.sync_logon_task();
            state.save_settings();
        }
        hover_tooltip("Only at logon. Manual launches always open normally.");
        global_badge();
    }

    if (state.settings.logon_action == app::LogonAction::BackgroundRefresh) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("Caches your master password via Windows DPAPI so the background run "
                           "can open the vault unattended. Anyone signed in as you on this PC can "
                           "then open the vault without the password.");
        ImGui::PopStyleColor();
    }

    ImGui::EndDisabled();

    if (!state.is_elevated) {
        ImGui::TextDisabled("Requires administrator.");
        ImGui::SameLine();
        if (action_button("Restart as administrator", ImVec2(210, 0))) restart_elevated(state);
    } else if (!s_logon_task_ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("Windows Task Scheduler refused the logon task. See the log for "
                           "the error code.");
        ImGui::PopStyleColor();
    }
}

void draw_notifications_section(app::AppState& state) {
    ImGui::Checkbox("Detect bans and cooldown changes", &state.settings.notifications.enabled);
    hover_tooltip("Each refresh compares against the previous snapshot. The first refresh "
                  "after adding an account only records the baseline.");
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
        hover_tooltip("Tray balloon for new bans and cooldowns. Suppressed while this "
                      "window is focused.");
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
    hover_tooltip("Above this many changes in one refresh, a single summary toast "
                  "replaces the per-account ones. Badges still appear.");
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Keep history (days)",
                     &state.settings.notifications.retention_days, 1, 365);
    ImGui::EndDisabled();
}

void draw_list_view_section(app::AppState& state) {
    separator_text("List view");
    ImGui::Checkbox("Show cooldown marker on rows",
                    &state.settings.list_view.show_cooldown_marker);
    hover_tooltip("Orange exclamation mark on rows currently on a CS2 cooldown.");
    ImGui::Checkbox("Show unread-event badge on rows",
                    &state.settings.list_view.show_unread_badge);
    hover_tooltip("Red exclamation mark on rows with unread ban or cooldown events.");
    ImGui::Checkbox("Show weekly-drop marker on rows",
                    &state.settings.list_view.show_weekly_drop_marker);
    hover_tooltip("Green checkmark on rows whose weekly XP drop is claimed.");
    ImGui::Checkbox("Hide account name in list",
                    &state.settings.list_view.hide_account_name);
    hover_tooltip("Hides the login name from rows. The persona name still shows.");
}

void draw_vault_section(app::AppState& state) {
    separator_text("Vault");
    {
        const bool prev = state.settings.remember_master_password;
        if (ImGui::Checkbox("Skip master-password prompt on launch (DPAPI)",
                            &state.settings.remember_master_password)) {
            if (state.settings.remember_master_password && !prev) {

                if (!settings_detail::write_master_pw_cache(state)) {
                    state.settings.remember_master_password = false;
                }
            } else if (!state.settings.remember_master_password && prev) {

                std::error_code ec;
                std::filesystem::remove(app::master_pw_cache_path(), ec);
                SAM_LOG_INFO("auto-unlock: DPAPI cache removed");
            }
            state.save_settings();
        }
    }
    hover_tooltip("Opens the vault automatically at launch. Anyone signed in as you on "
                  "this PC can then open it without the password.");
    global_badge();
}

bool category_item(const char* label, bool selected, float width) {
    const ImVec2 size{width, 32.0F};
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    ImGui::InvisibleButton("##cat", size);
    const bool pressed = ImGui::IsItemActivated();
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    auto* draw = ImGui::GetWindowDrawList();
    if (selected) {
        draw->AddRectFilled(cursor, ImVec2(cursor.x + size.x, cursor.y + size.y),
                            ImColor(255, 255, 255, 14), 6.0F);
        const ImVec4 accent = theme::accent();
        draw->AddRectFilled(ImVec2(cursor.x, cursor.y + 6.0F),
                            ImVec2(cursor.x + 3.0F, cursor.y + size.y - 6.0F),
                            ImColor(accent.x, accent.y, accent.z, 1.0F), 1.5F);
    } else if (hovered) {
        draw->AddRectFilled(cursor, ImVec2(cursor.x + size.x, cursor.y + size.y),
                            ImColor(255, 255, 255, 8), 6.0F);
    }

    const ImVec4 col = (selected || hovered) ? theme::text() : theme::dim_text();
    draw->AddText(ImVec2(cursor.x + 14.0F, cursor.y + 8.0F), ImColor(col), label);
    return pressed;
}

void render_general(app::AppState& state) {
    draw_updates_section(state);
    ImGui::Spacing();
    draw_admin_section(state);
    ImGui::Spacing();
    draw_startup_section(state);
}

void render_security(app::AppState& state) {
    draw_safe_mode_section(state);
    ImGui::Spacing();
    draw_clipboard_lock_section(state);
    ImGui::Spacing();
    draw_privacy_section(state);
}

void render_appearance(app::AppState& state) {
    draw_appearance_section(state);
    ImGui::Spacing();
    draw_account_info_section(state);
    ImGui::Spacing();
    draw_list_view_section(state);
}

void render_notifications(app::AppState& state) {
    draw_notifications_section(state);
}

void render_steam_guard(app::AppState& state) {
    settings_detail::draw_authenticator_section(state);
    ImGui::Spacing();
    settings_detail::draw_confirmations_section(state);
}

void render_launch_steam(app::AppState& state) {
    draw_steam_login_section(state);
    ImGui::Spacing();
    settings_detail::draw_cs2_config_section(state);
    if (state.settings.safe_mode) return;
    ImGui::Spacing();
    settings_detail::draw_gamesense_section(state);
    ImGui::Spacing();
    settings_detail::draw_luminary_section(state);
}

void render_cleaner(app::AppState& state) {
    settings_detail::draw_cleaner_section(state);
}

void render_network_data(app::AppState& state) {
    draw_integration_section(state);
    ImGui::Spacing();
    settings_detail::draw_proxy_section(state);
}

void render_cs2(app::AppState& state) {
    draw_cs2_gc_section(state);
}

void render_hwid(app::AppState& state) {
    if (ImGui::Checkbox("Always spoof HWID", &state.settings.hwid.always_spoof))
        state.save_settings();
    hover_tooltip("Spoofs hardware identifiers for every account on launch. "
                  "Exclude individual accounts from the right-click menu.");

    ImGui::Spacing();
    separator_text("Components");

    auto& mask = state.settings.hwid.component_mask;
    auto flag_checkbox = [&](const char* label, std::uint32_t bit) {
        bool on = (mask & bit) != 0;
        if (ImGui::Checkbox(label, &on)) {
            if (on) mask |= bit;
            else    mask &= ~bit;
            state.save_settings();
        }
    };

    const float grid_w = ImGui::GetContentRegionAvail().x - kContentPaddingX;
    if (ImGui::BeginTable("##hwid-comp-grid", 3, ImGuiTableFlags_SizingStretchSame,
            ImVec2(grid_w, 0.0f))) {
        ImGui::TableNextColumn(); flag_checkbox("Machine GUID",  0x001u);
        ImGui::TableNextColumn(); flag_checkbox("MAC address",   0x002u);
        ImGui::TableNextColumn(); flag_checkbox("Disk serial",   0x004u);

        ImGui::TableNextColumn(); flag_checkbox("PC name",       0x008u);
        ImGui::TableNextColumn(); flag_checkbox("GPU",           0x010u);
        ImGui::TableNextColumn(); flag_checkbox("Motherboard",   0x020u);

        ImGui::TableNextColumn(); flag_checkbox("RAM",           0x040u);
        ImGui::TableNextColumn(); flag_checkbox("Monitor",       0x080u);
        ImGui::TableNextColumn(); flag_checkbox("Storage",       0x100u);

        ImGui::TableNextColumn(); flag_checkbox("Sound card",    0x200u);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    separator_text("Hardware comparison");

    const auto* last_acc = state.last_hwid_account_id.empty()
        ? nullptr : state.find_account(state.last_hwid_account_id);
    if (!last_acc || !last_acc->hwid.has_value()) {
        ImGui::TextDisabled("No account has been spoofed this session.");
    } else {
        const auto& real = state.real_hardware;
        const auto& fake = *last_acc->hwid;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055F, 0.055F, 0.055F, 1.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));

        if (ImGui::BeginChild("##hwid-cmp-card", ImVec2(grid_w, 0),
                ImGuiChildFlags_AutoResizeY)) {

            const auto dim = theme::dim_text();

            if (ImGui::BeginTable("##hwid-cmp", 3,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX,
                    ImVec2(-1, 0.0f))) {
                ImGui::TableSetupColumn("##comp", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("##real");
                ImGui::TableSetupColumn("##spoofed");

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, dim);
                ImGui::TextUnformatted("Component");
                ImGui::TableNextColumn(); ImGui::TextUnformatted("Real");
                ImGui::TableNextColumn(); ImGui::TextUnformatted("Spoofed");
                ImGui::PopStyleColor();

                auto row = [&](const char* label, const std::string& r, const std::string& s) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, dim);
                    ImGui::TextUnformatted(label);
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(s.c_str());
                };
                auto row_int = [&](const char* label, int r, int s) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, dim);
                    ImGui::TextUnformatted(label);
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn(); ImGui::Text("%d", r);
                    ImGui::TableNextColumn(); ImGui::Text("%d", s);
                };

                row("Machine GUID", real.machine_guid, fake.machine_guid);
                row("MAC address",  real.mac_address,  fake.mac_address);
                row("Disk serial",  real.disk_serial,  fake.disk_serial);
                row("PC name",      real.pc_name,      fake.pc_name);
                row("GPU",          real.gpu_name,      fake.gpu_name);
                row("Board",        real.board_model,   fake.board_model);
                row("Board mfr",    real.board_manufacturer, fake.board_manufacturer);
                row_int("RAM (MB)",     real.ram_mb,        fake.ram_mb);
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, dim);
                    ImGui::TextUnformatted("Monitor");
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn(); ImGui::Text("%dx%d @%dHz", real.monitor_width, real.monitor_height, real.monitor_refresh);
                    ImGui::TableNextColumn(); ImGui::Text("%dx%d @%dHz", fake.monitor_width, fake.monitor_height, fake.monitor_refresh);
                }
                row("Sound card",   real.sound_card,    fake.sound_card);
                row("Display",      real.display_model, fake.display_model);
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
}

struct CategoryDef {
    const char* label;
    void (*render)(app::AppState&);

    bool unsafe = false;
};

void render_vaults(app::AppState& state) {
    settings_detail::draw_vaults_section(state);
    ImGui::Spacing();
    draw_vault_section(state);
    ImGui::Spacing();
    settings_detail::draw_storage_section(state);
}

constexpr CategoryDef kCategories[] = {
    {"General",            render_general},
    {"Appearance",         render_appearance},
    {"Notifications",      render_notifications},
    {"Steam Guard",        render_steam_guard},
    {"Launch & Steam",     render_launch_steam},
    {"CS2",                render_cs2},
    {"Network & Data",     render_network_data},
    {"Vaults",             render_vaults},
    {"Security & Privacy", render_security},
    {"Cleaner",            render_cleaner,  true},
    {"HWID Spoofer",       render_hwid,     true},
};

bool category_hidden(const app::AppState& state, int i) {
    return state.settings.safe_mode && kCategories[i].unsafe;
}

static_assert(IM_ARRAYSIZE(kCategories) == 11);
static_assert(static_cast<int>(SettingsCategory::NetworkData) == 6);
static_assert(static_cast<int>(SettingsCategory::Vaults) == 7);
static_assert(static_cast<int>(SettingsCategory::Cleaner) == 9);
static_assert(static_cast<int>(SettingsCategory::HwidSpoofer) == 10);

}  // namespace

constexpr float kFooterGap = 8.0F;

void draw_settings(app::AppState& state) {
    ImGui::TextUnformatted("Settings");
    ImGui::Spacing();

    constexpr float kButtonRowHeight = 26.0F;
    constexpr float kTrailingDummy   = 24.0F;
    const float footer_reserved = ImGui::GetStyle().ItemSpacing.y * 3.0F +
                                  kFooterGap + kButtonRowHeight + kTrailingDummy;

    ImGui::BeginChild("##settings-body", ImVec2(0, -footer_reserved),
                      ImGuiChildFlags_NavFlattened);

    static int active_category = 0;

    if (state.pending_settings_category >= 0 &&
        state.pending_settings_category < IM_ARRAYSIZE(kCategories) &&
        !category_hidden(state, state.pending_settings_category)) {
        active_category = state.pending_settings_category;
    }
    state.pending_settings_category = -1;

    if (category_hidden(state, active_category)) active_category = 0;
    constexpr float kRailWidth = 184.0F;

    ImGui::BeginChild("##settings-cats", ImVec2(kRailWidth, 0), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {

        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float wh = ImGui::GetWindowSize().y;
        draw->AddLine(ImVec2(wp.x + kRailWidth - 1.0F, wp.y),
                      ImVec2(wp.x + kRailWidth - 1.0F, wp.y + wh),
                      ImColor(0.659F, 0.635F, 0.620F, 0.10F));
    }
    ImGui::Dummy(ImVec2(0.0F, 4.0F));
    for (int i = 0; i < IM_ARRAYSIZE(kCategories); ++i) {
        if (category_hidden(state, i)) continue;
        ImGui::SetCursorPosX(8.0F);
        if (category_item(kCategories[i].label, active_category == i, kRailWidth - 16.0F)) {
            active_category = i;
        }
        ImGui::Dummy(ImVec2(0.0F, 4.0F));
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0F, 16.0F);

    ImGui::BeginChild("##settings-content", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);

    ImGui::PushItemWidth(-kContentPaddingX);

    ImFont* tf = fonts::title();
    if (tf) ImGui::PushFont(tf, 0.0F);
    ImGui::TextUnformatted(kCategories[active_category].label);
    if (tf) ImGui::PopFont();
    ImGui::Spacing();

    kCategories[active_category].render(state);

    ImGui::PopItemWidth();
    ImGui::EndChild();

    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0.0F, kFooterGap));
    if (action_button("Save settings", ImVec2(160, 0))) {
        state.save_settings();
        ImGui::OpenPopup("Settings saved");
    }

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
