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
#include "platform/window_affinity.hpp"
#include "ui/fonts.hpp"
#include "ui/screens/settings_sections.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::screens {

namespace {

void draw_clipboard_lock_section(app::AppState& state) {
    separator_text("Clipboard & auto-lock");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Clipboard auto-clear (s)", &state.settings.clipboard_clear_seconds, 10, 120);
    hover_tooltip("Passwords and Steam Guard codes copied from the app are wiped from the "
                  "clipboard after this many seconds.");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Auto-lock (minutes)", &state.settings.auto_lock_minutes, 0, 240);
    hover_tooltip("Re-lock the vault after this many idle minutes. 0 disables auto-lock for the "
                  "current session.");
}

void draw_updates_section(app::AppState& state) {
    separator_text("Updates");
    ImGui::Checkbox("Check for updates on launch", &state.settings.check_updates_on_launch);
    hover_tooltip("On launch, checks GitHub for a newer release and shows an \"Update available\" "
                  "prompt if one exists. No account data is sent.");
}

void draw_appearance_section(app::AppState& state) {
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

void draw_steam_login_section(app::AppState& state) {
    separator_text("Steam login");

    {
        int method = static_cast<int>(state.settings.sign_in_method);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Sign-in method", &method,
                         "Sign-in driver\0Token injection\0")) {
            state.settings.sign_in_method = static_cast<app::SignInMethod>(method);
            state.save_settings();
        }
        hover_tooltip("Sign-in driver: automates the Steam login window (types password and "
                      "Steam Guard code). Token injection: writes a saved login token directly "
                      "into Steam's config so it auto-signs in without a login window. Token "
                      "injection requires a stored password to mint the token on first use. "
                      "NFA accounts always use token injection regardless of this setting.");
    }

    if (ImGui::Checkbox("Disable Steam Cloud on login",
                        &state.settings.disable_cloud_on_login)) {
        state.save_settings();
    }
    hover_tooltip("When you launch an account, set CloudEnabled=0 for it (Steam's \"Enable "
                  "Steam Cloud\" off). Applies to every account you launch. Turning this off "
                  "does not re-enable cloud; it just stops the app touching the file. Note: "
                  "this flips the account's account-wide cloud setting.");

    if (ImGui::Checkbox("Disable new-release news on login",
                        &state.settings.disable_news_on_login)) {
        state.save_settings();
    }
    hover_tooltip("When you launch an account, set NotifyAvailableGames=0 (Steam's \"Notify "
                  "me about additions or changes to my games, new releases\" off). Applies to "
                  "every account you launch; turning it off leaves Steam's setting as-is.");

    if (ImGui::Checkbox("Disable CS2 workshop map downloads on login",
                        &state.settings.disable_workshop_on_login)) {
        state.save_settings();
    }
    hover_tooltip("When you launch an account, stop its subscribed CS2 workshop maps from "
                  "downloading: the shared appworkshop_730.acf is stripped of that account's "
                  "not-yet-installed items and locked read-only. Applies to every account you "
                  "launch. Subscriptions are kept; turning it off unlocks the file on the next "
                  "launch. Note: the acf is install-wide, so while on it also pauses workshop "
                  "downloads for your other accounts.");

    if (ImGui::Button("Re-enable Steam workshop downloads")) {
        const auto r = cs2_config::unlock_workshop_block();
        SAM_LOG_INFO("settings: unlock workshop downloads -> ok={} ({})", r.ok, r.message);
    }
    hover_tooltip("Manually clears the read-only lock the option above places on Steam's "
                  "appworkshop_730.acf, so Steam downloads subscribed CS2 workshop maps "
                  "normally again. Launching an account with the toggle off also clears it.");

    {
        const bool token_mode =
            state.settings.sign_in_method == app::SignInMethod::TokenInject;
        ImGui::BeginDisabled(token_mode);
        if (ImGui::Checkbox("Remember password on login",
                            &state.settings.remember_password_on_login)) {
            state.save_settings();
        }
        hover_tooltip("Ticks Steam's \"Remember me\" box when signing a password account in, so "
                      "Steam keeps the saved login. Turn off to have Steam forget the session "
                      "after sign-in. Token injection always needs a remembered session, so this "
                      "is ignored when using that method.");
        ImGui::EndDisabled();
    }
}

void draw_cs2_gc_section(app::AppState& state) {
    separator_text("CS2 Game Coordinator");
    ImGui::Checkbox("Enable CS2 tab", &state.settings.cs2_gc.enabled);
    hover_tooltip("Shows the CS2 tab in the sidebar and lets the app open a live Game "
                  "Coordinator connection to manage inventory and weekly drops. Turning this "
                  "off hides the tab and closes any active connection.");
    ImGui::BeginDisabled(!state.settings.cs2_gc.enabled);
    ImGui::Checkbox("Show weekly drop card", &state.settings.cs2_gc.show_weekly_drop);
    ImGui::Checkbox("Show weekly mission card", &state.settings.cs2_gc.show_weekly_mission);
    ImGui::Checkbox("Show inventory card", &state.settings.cs2_gc.show_inventory);
    ImGui::Checkbox("Show storage units card", &state.settings.cs2_gc.show_storage_units);
    ImGui::Checkbox("Mark weekly drop claimed automatically",
                    &state.settings.cs2_gc.auto_mark_claimed);
    hover_tooltip("When connected, keep the account-list weekly-drop marker in step with the "
                  "GC: light it once this week's drop is gone, and clear it when a new week "
                  "resets. Off leaves the marker to the manual \"Mark claimed\" action only.");
    ImGui::Checkbox("Auto-pull GC data on startup",
                    &state.settings.cs2_gc.auto_pull_on_startup);
    hover_tooltip("On launch, automatically pull CS2 GC data (medals, level, weekly drop) for "
                  "every eligible account, signing in where needed. Skips the account signed in "
                  "to Steam on this PC and any whose cache is still fresh, so a restart within "
                  "the cache window does nothing.");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("GC cache duration (hours)", &state.settings.cs2_gc.cache_hours, 1, 24);
    hover_tooltip("How long a pulled account stays cached before auto-pull will refresh it. "
                  "The cache is saved to the vault, so it survives restarts.");
    ImGui::EndDisabled();
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
    ImGui::SameLine();
    if (action_button("Get key")) {
        open_url("https://steamcommunity.com/dev/apikey");
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

    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Steam data cache (hours)", &state.settings.steam_cache_hours, 1, 24);
    hover_tooltip("Global 'refresh only if older than X' for the Steam Web API data (bans, "
                  "level, games, summary) -- any batch/startup/auto refresh skips accounts "
                  "newer than this, like the GC cache. A manual single-account Refresh forces.");

    separator_text("Auto-refresh");
    ImGui::Checkbox("Auto-refresh while open", &state.settings.auto_refresh_enabled);
    hover_tooltip("Every X minutes, refresh each account whose Steam or GC cache has expired "
                  "(medals, ranks, and NFA/cached token validation), skipping the rest. Never "
                  "runs the balance or GCPD scrapes.");
    ImGui::BeginDisabled(!state.settings.auto_refresh_enabled);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Refresh interval (minutes)", &state.settings.auto_refresh_minutes, 10, 720);
    ImGui::EndDisabled();
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

// Category button for the settings sub-rail, styled like the main rail nav's
// sidebar_item (rail_nav.cpp): selected gets a faint fill + accent left bar,
// hover gets a fainter fill. Returns true on click.
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
    draw_startup_section(state);
    ImGui::Spacing();
    settings_detail::draw_storage_section(state);
}

void render_security(app::AppState& state) {
    draw_clipboard_lock_section(state);
    ImGui::Spacing();
    draw_vault_section(state);
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
    ImGui::Spacing();
    settings_detail::draw_gamesense_section(state);
    ImGui::Spacing();
    settings_detail::draw_luminary_section(state);
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
    hover_tooltip("Automatically spoof hardware identifiers for every account on launch.\n"
                  "Excluded accounts can be set via the right-click menu.");

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
};

constexpr CategoryDef kCategories[] = {
    {"General",            render_general},
    {"Security & Privacy", render_security},
    {"Appearance",         render_appearance},
    {"Notifications",      render_notifications},
    {"Steam Guard",        render_steam_guard},
    {"Launch & Steam",     render_launch_steam},
    {"Network & Data",     render_network_data},
    {"CS2",                render_cs2},
    {"HWID Spoofer",       render_hwid},
};

// Keep SettingsCategory (header) in lockstep with kCategories order/count.
static_assert(IM_ARRAYSIZE(kCategories) == 9);
static_assert(static_cast<int>(SettingsCategory::NetworkData) == 6);

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

    // Selected category for the sub-rail. Transient view state, kept for the
    // session only (not persisted to settings).
    static int active_category = 0;
    // A one-shot external request (e.g. the missing-key toast) can jump to a tab.
    if (state.pending_settings_category >= 0 &&
        state.pending_settings_category < IM_ARRAYSIZE(kCategories)) {
        active_category = state.pending_settings_category;
    }
    state.pending_settings_category = -1;
    constexpr float kRailWidth = 184.0F;

    ImGui::BeginChild("##settings-cats", ImVec2(kRailWidth, 0), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        // Faint divider on the rail's right edge, matching the main rail nav.
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float wh = ImGui::GetWindowSize().y;
        draw->AddLine(ImVec2(wp.x + kRailWidth - 1.0F, wp.y),
                      ImVec2(wp.x + kRailWidth - 1.0F, wp.y + wh),
                      ImColor(0.659F, 0.635F, 0.620F, 0.10F));
    }
    ImGui::Dummy(ImVec2(0.0F, 4.0F));
    for (int i = 0; i < IM_ARRAYSIZE(kCategories); ++i) {
        ImGui::SetCursorPosX(8.0F);
        if (category_item(kCategories[i].label, active_category == i, kRailWidth - 16.0F)) {
            active_category = i;
        }
        ImGui::Dummy(ImVec2(0.0F, 4.0F));
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0F, 16.0F);

    ImGui::BeginChild("##settings-content", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    // PushItemWidth doesn't carry across the child boundary; re-apply so right edges match.
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
