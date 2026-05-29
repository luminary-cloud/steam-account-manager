#include "ui/screens/settings_screen.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

#include <windows.h>

#include <imgui.h>

#include "app/app_paths.hpp"
#include "core/log.hpp"
#include "platform/dpapi.hpp"
#include "platform/file_dialog.hpp"
#include "platform/fs.hpp"
#include "platform/startup_task.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::screens {

void draw_settings(app::AppState& state) {
    ImGui::TextUnformatted("Settings");
    ImGui::Spacing();

    // Writes the DPAPI-wrapped master password so the next launch (or the
    // headless logon run) can open the vault without a prompt. Returns false if
    // the password isn't available or the write fails.
    auto write_master_pw_cache = [&state]() -> bool {
        try {
            std::span<const std::uint8_t> pw_bytes{
                reinterpret_cast<const std::uint8_t*>(state.master_password.data()),
                state.master_password.size()};
            auto wrapped = sam::platform::dpapi::protect(pw_bytes);
            sam::platform::atomic_write_file(app::master_pw_cache_path(), wrapped);
            SAM_LOG_INFO("auto-unlock: DPAPI cache written");
            return true;
        } catch (const std::exception& ex) {
            SAM_LOG_ERROR("auto-unlock: cache write failed: {}", ex.what());
            return false;
        }
    };

    ImGui::SeparatorText("General");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Clipboard auto-clear (s)", &state.settings.clipboard_clear_seconds, 0, 60);
    hover_tooltip("Codes copied via the Authenticator screen are wiped from the clipboard after "
                  "this many seconds. 0 disables the auto-wipe.");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Auto-lock (minutes)", &state.settings.auto_lock_minutes, 0, 240);
    hover_tooltip("Re-lock the vault after this many idle minutes. 0 disables auto-lock for the "
                  "current session.");
    ImGui::Checkbox("Check for updates on launch", &state.settings.check_updates_on_launch);
    hover_tooltip("On launch, checks GitHub for a newer release and shows an \"Update available\" "
                  "prompt if one exists. No account data is sent.");

    ImGui::Spacing();
    ImGui::SeparatorText("Appearance");
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

    ImGui::Spacing();
    ImGui::SeparatorText("Privacy");
    if (ImGui::Checkbox("Privacy mode - hide login names", &state.settings.privacy_mode)) {
        // Switching the toggle - either direction - resets any per-account
        // reveals so the new mode starts from a clean state.
        state.clear_session_secrets();
    }
    hover_tooltip("Replaces account login names with <hidden> everywhere they appear. Click a "
                  "redacted name to reveal that one account for the rest of the session; click "
                  "again to re-hide. Reveals are cleared when the vault locks.");

    ImGui::Spacing();
    ImGui::SeparatorText("Account info to display");
    if (ImGui::BeginTable("##info-grid", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); ImGui::Checkbox("VAC ban",            &state.settings.info.show_vac);
        ImGui::TableNextColumn(); ImGui::Checkbox("Game ban",           &state.settings.info.show_game_ban);
        ImGui::TableNextColumn(); ImGui::Checkbox("Community ban",      &state.settings.info.show_community_ban);

        ImGui::TableNextColumn(); ImGui::Checkbox("Trade ban",          &state.settings.info.show_trade_ban);
        ImGui::TableNextColumn(); ImGui::Checkbox("Steam level",        &state.settings.info.show_steam_level);
        ImGui::TableNextColumn(); ImGui::Checkbox("Owned games count",  &state.settings.info.show_owned_games);

        // Premier/Wingman/MapRanks/Prime/Cooldown are populated by the
        // GCPD scraper (gcpd_enabled below). Requires a valid login so
        // the steamLoginSecure cookie is fresh; see Full Login wizard.
        ImGui::TableNextColumn(); ImGui::Checkbox("Premier rating",     &state.settings.info.show_premier);
        ImGui::TableNextColumn(); ImGui::Checkbox("Wingman rank",       &state.settings.info.show_wingman);

        ImGui::TableNextColumn(); ImGui::Checkbox("Prime status",       &state.settings.info.show_prime);
        ImGui::TableNextColumn(); ImGui::Checkbox("VAC-Live indicator", &state.settings.info.show_vac_live);
        ImGui::TableNextColumn(); ImGui::Checkbox("Cooldown countdown", &state.settings.info.show_cooldown);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Integration");
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

    ImGui::Spacing();
    ImGui::SeparatorText("Startup");
    {
        const bool prev = state.settings.start_with_windows;
        if (ImGui::Checkbox("Start with Windows (refresh in the background at logon)",
                            &state.settings.start_with_windows)) {
            if (state.settings.start_with_windows && !prev) {
                // Background refresh needs the vault to auto-unlock, so enable
                // refresh-on-launch and the DPAPI password cache alongside it.
                state.settings.refresh_on_launch = true;
                if (write_master_pw_cache()) state.settings.remember_master_password = true;
                if (!state.settings.remember_master_password ||
                    !sam::platform::startup_task::set_run_at_logon(true)) {
                    SAM_LOG_ERROR("startup: failed to enable start-with-Windows");
                    state.settings.start_with_windows = false;
                }
            } else if (!state.settings.start_with_windows && prev) {
                sam::platform::startup_task::set_run_at_logon(false);
            }
            state.save_settings();
        }
    }
    hover_tooltip("Registers a Scheduled Task that runs this app hidden at logon with admin "
                  "rights, refreshes every account, shows a Windows notification for any new ban "
                  "or cooldown, then exits. Also turns on Refresh on launch and the master-"
                  "password cache.");
    ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
    ImGui::TextWrapped("Caches your master password via Windows DPAPI so the background run can "
                       "open the vault unattended. Anyone signed in as you on this PC can then "
                       "open the vault without the password.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::SeparatorText("Notifications");
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

    ImGui::Spacing();
    ImGui::SeparatorText("Authenticator");
    ImGui::Checkbox("Auto-copy code on account select",
                    &state.settings.sda.auto_copy_on_select);
    hover_tooltip("Copies the current Steam Guard code to the clipboard whenever you "
                  "pick a different account in the Authenticator picker. Clipboard "
                  "auto-clear above still applies.");
    ImGui::Checkbox("Show next code preview",
                    &state.settings.sda.show_next_code);
    hover_tooltip("Dim text under the current code showing what it will rotate to "
                  "in the next 30-second window.");
    ImGui::Checkbox("Hide current code (click to reveal)",
                    &state.settings.sda.hide_current_code);
    hover_tooltip("Hides the code until you click it. Reveals for 5 seconds, then re-hides.");

    if (ImGui::Checkbox("Global hotkey to copy current code",
                        &state.settings.sda.global_hotkey_enabled)) {
        state.needs_hotkey_reregister = true;
        state.save_settings();
    }
    hover_tooltip("Registers a system-wide shortcut that copies the current Steam Guard "
                  "code for the most recently selected account, even when this app isn't "
                  "focused. Default is Ctrl + Shift + G.");
    {
        auto mods_text = [](std::uint32_t m) {
            std::string s;
            if (m & MOD_CONTROL) s += "Ctrl + ";
            if (m & MOD_SHIFT)   s += "Shift + ";
            if (m & MOD_ALT)     s += "Alt + ";
            if (m & MOD_WIN)     s += "Win + ";
            return s;
        };
        auto vk_text = [](std::uint32_t vk) {
            char buf[8];
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
                std::snprintf(buf, sizeof(buf), "%c", static_cast<char>(vk));
            } else if (vk >= VK_F1 && vk <= VK_F12) {
                std::snprintf(buf, sizeof(buf), "F%u",
                              static_cast<unsigned>(vk - VK_F1 + 1));
            } else {
                std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(vk));
            }
            return std::string{buf};
        };

        static bool g_capturing_hotkey = false;
        const std::string current = mods_text(state.settings.sda.global_hotkey_mods) +
                                     vk_text(state.settings.sda.global_hotkey_vk);
        ImGui::BeginDisabled(!state.settings.sda.global_hotkey_enabled);
        ImGui::TextDisabled("Binding:");
        ImGui::SameLine();
        ImGui::TextUnformatted(current.c_str());
        ImGui::SameLine();
        if (g_capturing_hotkey) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextUnformatted("(press a key combination)");
            ImGui::PopStyleColor();
        } else {
            if (ImGui::SmallButton("Change...")) {
                g_capturing_hotkey = true;
            }
        }

        if (g_capturing_hotkey) {
            ImGuiIO& io = ImGui::GetIO();
            std::uint32_t mods = 0;
            if (io.KeyCtrl)  mods |= MOD_CONTROL;
            if (io.KeyShift) mods |= MOD_SHIFT;
            if (io.KeyAlt)   mods |= MOD_ALT;
            if (io.KeySuper) mods |= MOD_WIN;

            auto try_capture = [&](ImGuiKey first, ImGuiKey last,
                                    std::uint32_t vk_base) -> bool {
                for (int k = first; k <= last; ++k) {
                    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
                        state.settings.sda.global_hotkey_mods = mods;
                        state.settings.sda.global_hotkey_vk   = vk_base + (k - first);
                        state.needs_hotkey_reregister = true;
                        state.save_settings();
                        g_capturing_hotkey = false;
                        return true;
                    }
                }
                return false;
            };
            (void)(try_capture(ImGuiKey_A,  ImGuiKey_Z, 'A') ||
                   try_capture(ImGuiKey_0,  ImGuiKey_9, '0') ||
                   try_capture(ImGuiKey_F1, ImGuiKey_F12, VK_F1) ||
                   (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ?
                    (g_capturing_hotkey = false, true) : false));
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("List view");
    ImGui::Checkbox("Show cooldown marker on rows",
                    &state.settings.list_view.show_cooldown_marker);
    hover_tooltip("Adds an orange exclamation mark to the right of any list row whose "
                  "account is currently on a CS2 competitive cooldown.");
    ImGui::Checkbox("Show unread-event badge on rows",
                    &state.settings.list_view.show_unread_badge);
    hover_tooltip("Adds a red exclamation mark to the right of any list row that has "
                  "un-acknowledged ban or cooldown notifications.");
    ImGui::Checkbox("Hide account name in list",
                    &state.settings.list_view.hide_account_name);
    hover_tooltip("Hides the Steam login/account name from account-list rows. The persona name "
                  "still shows, and the selected account's detail panel is unaffected.");

    ImGui::Spacing();
    ImGui::SeparatorText("Confirmations");
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Per-account refresh cooldown (s)",
                     &state.settings.confirmations.per_account_cooldown_seconds, 0, 300);
    hover_tooltip("Min seconds between confirmation-list refreshes for the same account. "
                  "Prevents accidental spamming of Steam's /mobileconf endpoint.");
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Refresh-all stagger (ms)",
                     &state.settings.confirmations.refresh_stagger_ms, 0, 2000);
    hover_tooltip("Delay between submissions when you click Refresh all. With 100+ accounts "
                  "this prevents Steam from rate-limiting the whole IP.");
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Permanent-failure after N session errors",
                     &state.settings.confirmations.permanent_failure_threshold, 1, 10);
    hover_tooltip("After this many consecutive session errors, the account is skipped by "
                  "Refresh all and the background poller. Manual refresh still works and "
                  "clears the mark on success.");
    ImGui::Checkbox("Background poll for new confirmations",
                    &state.settings.confirmations.background_poll_enabled);
    hover_tooltip("Wakes a background thread every N minutes to run Refresh all. "
                  "Skips accounts marked permanent-failure and respects the per-account cooldown.");
    ImGui::BeginDisabled(!state.settings.confirmations.background_poll_enabled);
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Poll interval (minutes)",
                     &state.settings.confirmations.background_poll_minutes, 1, 120);
    ImGui::EndDisabled();

    ImGui::Checkbox("Toast when new confirmations arrive",
                    &state.settings.confirmations.toast_on_new_confirmations);
    ImGui::Checkbox("Show search bar above the account list",
                    &state.settings.confirmations.show_account_search);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
    ImGui::TextWrapped("Auto-approve is policy that can lose money. The app never auto-denies. "
                       "Trade approvals require a steamid whitelist (added later). Off by default.");
    ImGui::PopStyleColor();
    ImGui::Checkbox("Auto-approve enabled",
                    &state.settings.confirmations.auto_approve_enabled);
    ImGui::BeginDisabled(!state.settings.confirmations.auto_approve_enabled);
    ImGui::Checkbox("...market listings",
                    &state.settings.confirmations.auto_approve_market);
    ImGui::Checkbox("...phone-number-change confirmations",
                    &state.settings.confirmations.auto_approve_phone_change);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SetNextItemWidth(180);
    ImGui::SliderInt("Audit log retention (days)",
                     &state.settings.confirmations.audit_retention_days, 1, 730);

    ImGui::Spacing();
    ImGui::SeparatorText("Vault");
    {
        const bool prev = state.settings.remember_master_password;
        if (ImGui::Checkbox("Skip master-password prompt on launch (DPAPI)",
                            &state.settings.remember_master_password)) {
            if (state.settings.remember_master_password && !prev) {
                // Just enabled: write the cache now using the unlocked master
                // password. If we can't, revert the toggle.
                if (!write_master_pw_cache()) {
                    state.settings.remember_master_password = false;
                }
            } else if (!state.settings.remember_master_password && prev) {
                // Just disabled: delete the cache so the next launch prompts.
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

    ImGui::Spacing();
    ImGui::SeparatorText("CS2 video config");
    ImGui::Checkbox("Apply video config on login",
                    &state.settings.cs2_video.auto_apply_on_login);
    hover_tooltip("When you press Login on an account, copy the stored cs2_video.txt into that "
                  "account's CS2 config folder (userdata/<id>/730/local/cfg/cs2_video.txt). Any "
                  "existing file is backed up first. Requires a template chosen below.");
    {
        ImGui::TextUnformatted("Template:");
        ImGui::SameLine();
        if (state.settings.cs2_video.source_label.empty()) {
            ImGui::TextDisabled("none selected");
        } else {
            ImGui::TextDisabled("%s", state.settings.cs2_video.source_label.c_str());
        }
        if (action_button("Choose video.txt...", ImVec2(160, 0))) {
            platform::file_dialog::Options opts;
            opts.parent = state.main_hwnd;
            opts.title = L"Select CS2 video.txt";
            opts.filters = {{L"Video config (*.txt)", L"*.txt"},
                            {L"All files (*.*)", L"*.*"}};
            const auto res = platform::file_dialog::open_file(opts);
            if (res.ok) {
                // Keep our own copy so the imported file survives the original
                // being moved or deleted.
                std::error_code ec;
                std::filesystem::copy_file(res.path, app::cs2_video_template_path(),
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    SAM_LOG_ERROR("cs2 video template import failed: {}", ec.message());
                } else {
                    state.settings.cs2_video.source_label = res.path.string();
                    state.save_settings();
                }
            }
        }
        if (!state.settings.cs2_video.source_label.empty()) {
            ImGui::SameLine();
            if (action_button("Clear", ImVec2(80, 0))) {
                std::error_code ec;
                std::filesystem::remove(app::cs2_video_template_path(), ec);
                state.settings.cs2_video.source_label.clear();
                state.save_settings();
            }
        }
    }
    hover_tooltip("The chosen file is copied to the app data folder and used as the single "
                  "default for every account.");

    ImGui::Spacing();
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
