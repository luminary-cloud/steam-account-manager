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
#include "app/gamesense_loader.hpp"
#include "app/job_pump.hpp"
#include "core/cs2_config/video_config.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/http/client.hpp"
#include "core/log.hpp"
#include "platform/dpapi.hpp"
#include "platform/file_dialog.hpp"
#include "platform/fs.hpp"
#include "platform/paths.hpp"
#include "platform/startup_task.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/master_pw_field.hpp"

namespace sam::ui::screens {

// Gap above the pinned Save / Open-data-folder footer row.
constexpr float kFooterGap = 8.0F;

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

    // Pin the footer to the bottom: the body scrolls inside a child that reserves
    // room for everything drawn after it, so the parent content region doesn't
    // grow a second scrollbar. That trailing content is the gap + button row here
    // plus the three item-spacings around them and main_window's Dummy(0,24) added
    // after this screen returns.
    constexpr float kButtonRowHeight = 26.0F;  // action_button height
    constexpr float kTrailingDummy   = 24.0F;  // main_window's Dummy after draw_screen
    const float footer_reserved = ImGui::GetStyle().ItemSpacing.y * 3.0F +
                                  kFooterGap + kButtonRowHeight + kTrailingDummy;

    ImGui::BeginChild("##settings-body", ImVec2(0, -footer_reserved),
                      ImGuiChildFlags_NavFlattened);
    // The parent's PushItemWidth(-kContentPaddingX) doesn't carry across the
    // child boundary; re-apply it so widget and separator right edges match.
    ImGui::PushItemWidth(-kContentPaddingX);

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

    ImGui::Spacing();
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

    ImGui::Spacing();
    separator_text("Privacy");
    if (ImGui::Checkbox("Privacy mode - hide login names", &state.settings.privacy_mode)) {
        // Switching the toggle - either direction - resets any per-account
        // reveals so the new mode starts from a clean state.
        state.clear_session_secrets();
    }
    hover_tooltip("Replaces account login names with <hidden> everywhere they appear. Click a "
                  "redacted name to reveal that one account for the rest of the session; click "
                  "again to re-hide. Reveals are cleared when the vault locks.");

    ImGui::Spacing();
    separator_text("Account info to display");
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
        ImGui::TableNextColumn(); ImGui::Checkbox("Weekly XP drop",     &state.settings.info.show_weekly_drop);
        ImGui::TableNextColumn(); ImGui::Checkbox("External funds",     &state.settings.info.show_external_funds);

        ImGui::EndTable();
    }

    ImGui::Spacing();
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

    ImGui::Spacing();
    separator_text("Proxy");

    // Shared async probe for both Test buttons: forces the given proxy regardless of
    // the active mode and reports the exit IP (or the transport error).
    static std::string single_test_result;
    static bool        single_testing = false;
    static std::string acct_test_result;
    static bool        acct_testing = false;
    auto run_proxy_test = [&state](std::string proxy, std::string* result, bool* testing) {
        if (proxy.empty()) { *result = "Enter a proxy first."; return; }
        *testing = true;
        *result = "Testing...";
        app::job_pump::submit([proxy, result, testing, &state] {
            http::ScopedProxyOverride ov(proxy);
            http::Request req;
            req.url = "https://api.ipify.org";
            req.timeout_seconds = 20;
            req.max_retries = 1;
            const auto resp = http::request(req);
            std::string msg;
            if (resp.status == 200 && !resp.body.empty()) {
                msg = "Exit IP: " + resp.body;
            } else if (resp.transport_error) {
                msg = "Failed: " + resp.error_message;
            } else {
                msg = "Failed (HTTP " + std::to_string(resp.status) + ")";
            }
            state.post_ui_callback([result, testing, msg = std::move(msg)] {
                *result = msg;
                *testing = false;
            });
        });
    };

    {
        int mode_idx = static_cast<int>(state.settings.proxy_mode);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Proxy mode", &mode_idx, "None\0Single proxy\0Per-account\0")) {
            state.settings.proxy_mode = static_cast<app::ProxyMode>(mode_idx);
            state.sync_proxy_policy();
            state.save_settings();
        }
        hover_tooltip("None: every account connects directly (default). Single proxy: one proxy "
                      "for all accounts. Per-account: each account uses its own proxy; accounts "
                      "without one stay direct. Applies to the app's web traffic, not the launched "
                      "Steam client.");

        if (state.settings.proxy_mode == app::ProxyMode::Single) {
            if (widgets::draw_password_field("Proxy URL##single-proxy",
                                             state.settings.single_proxy, false, 300.0F)) {
                state.sync_proxy_policy();
                state.save_settings();
            }
            hover_tooltip("scheme://[user:pass@]host:port  e.g. "
                          "socks5://user:pass@host:1080 (http/https also work).");
            ImGui::BeginDisabled(single_testing || state.settings.single_proxy.empty());
            if (action_button("Test##single-proxy", ImVec2(80, 0))) {
                run_proxy_test(state.settings.single_proxy, &single_test_result, &single_testing);
            }
            ImGui::EndDisabled();
            if (!single_test_result.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", single_test_result.c_str());
            }
        } else if (state.settings.proxy_mode == app::ProxyMode::PerAccount) {
            if (action_button("Manage per-account proxies...", ImVec2(220, 0))) {
                ImGui::OpenPopup("Per-account proxies");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Accounts without a proxy use a direct connection.");
        }
    }

    // Per-account proxy editor (rendered every frame; a no-op until opened above).
    {
        static std::string sel_id;
        static std::string proxy_buf;
        static std::string loaded_for;
        static std::string acct_save_msg;

        if (begin_styled_modal("Per-account proxies", 640.0F)) {
            ImGui::TextDisabled("Pick an account, set its proxy, then Save. Blank = direct. "
                                "A leading * marks accounts that already have one.");
            ImGui::Spacing();

            ImGui::BeginChild("##proxy-acct-list", ImVec2(240.0F, 300.0F), true);
            for (const auto& a : state.vault.accounts) {
                std::string label = a.proxy.empty() ? "   " : "* ";
                label += a.web.persona_name.empty() ? a.login : a.web.persona_name;
                label += "##acct-" + a.id;
                if (ImGui::Selectable(label.c_str(), sel_id == a.id)) {
                    sel_id = a.id;
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginGroup();
            if (auto* a = state.find_account(sel_id)) {
                if (loaded_for != sel_id) {
                    proxy_buf = std::string(a->proxy.begin(), a->proxy.end());
                    loaded_for = sel_id;
                    acct_test_result.clear();
                    acct_save_msg.clear();
                }
                ImGui::TextUnformatted(a->web.persona_name.empty()
                                           ? a->login.c_str()
                                           : a->web.persona_name.c_str());
                ImGui::Spacing();
                ImGui::TextUnformatted("Proxy");
                widgets::draw_password_field("##acct-proxy", proxy_buf, false, 250.0F);
                hover_tooltip("scheme://[user:pass@]host:port (socks5, http, https). "
                              "Leave blank for a direct connection.");
                ImGui::Spacing();
                if (action_button("Save##acct-proxy", ImVec2(80, 0))) {
                    if (auto* acc = state.find_account(sel_id)) {
                        acc->proxy = crypto::make_secure(proxy_buf);
                        state.vault_dirty = true;
                        state.save_vault_if_dirty();
                        acct_save_msg = "Saved.";
                    }
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(acct_testing || proxy_buf.empty());
                if (action_button("Test##acct-proxy", ImVec2(80, 0))) {
                    run_proxy_test(proxy_buf, &acct_test_result, &acct_testing);
                }
                ImGui::EndDisabled();
                if (!acct_save_msg.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", acct_save_msg.c_str());
                }
                if (!acct_test_result.empty()) {
                    ImGui::TextDisabled("%s", acct_test_result.c_str());
                }
            } else {
                ImGui::TextDisabled("Select an account on the left.");
            }
            ImGui::EndGroup();

            ImGui::Spacing();
            ImGui::Separator();
            if (action_button("Close", ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
            }
            end_styled_modal();
        }
    }

    ImGui::Spacing();
    separator_text("Startup");
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

    ImGui::Spacing();
    separator_text("Authenticator");
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

    ImGui::Spacing();
    separator_text("Confirmations");
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
    separator_text("Vault");
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
    separator_text("CS2 config on login");
    {
        int mode_idx = static_cast<int>(state.settings.cs2_video.mode);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("CS2 config mode", &mode_idx,
                         "None\0Video txt only\0Whole 730 folder\0")) {
            state.settings.cs2_video.mode = static_cast<app::CS2ConfigMode>(mode_idx);
            state.save_settings();
        }
        hover_tooltip("None: nothing on login. Video txt only: copy cs2_video.txt into "
                      "userdata/<id>/730/local/cfg. Whole 730 folder: copy an entire 730 "
                      "folder into userdata/<id>/730 (files merged over any existing ones).");
    }

    if (state.settings.cs2_video.mode == app::CS2ConfigMode::VideoTxt) {
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
            if (action_button("Clear##cs2-video", ImVec2(80, 0))) {
                std::error_code ec;
                std::filesystem::remove(app::cs2_video_template_path(), ec);
                state.settings.cs2_video.source_label.clear();
                state.save_settings();
            }
        }
        hover_tooltip("The chosen file is copied to the app data folder and used as the single "
                      "default for every account.");
    } else if (state.settings.cs2_video.mode == app::CS2ConfigMode::Folder730) {
        ImGui::TextUnformatted("Source 730 folder:");
        ImGui::SameLine();
        if (state.settings.cs2_video.folder_source_label.empty()) {
            ImGui::TextDisabled("none selected");
        } else {
            ImGui::TextDisabled("%s", state.settings.cs2_video.folder_source_label.c_str());
        }
        if (action_button("Choose 730 folder...", ImVec2(160, 0))) {
            platform::file_dialog::Options opts;
            opts.parent = state.main_hwnd;
            opts.title = L"Select CS2 730 folder";
            const auto res = platform::file_dialog::pick_folder(opts);
            if (res.ok) {
                // Snapshot the folder into app data so it survives the original
                // being moved or deleted.
                const auto imp = cs2_config::import_730_template(
                    res.path, app::cs2_730_template_dir());
                if (!imp.ok) {
                    SAM_LOG_ERROR("cs2 730 template import failed: {}", imp.message);
                } else {
                    state.settings.cs2_video.folder_source_label = res.path.string();
                    state.save_settings();
                }
            }
        }
        if (!state.settings.cs2_video.folder_source_label.empty()) {
            ImGui::SameLine();
            if (action_button("Clear##cs2-730", ImVec2(80, 0))) {
                std::error_code ec;
                std::filesystem::remove_all(app::cs2_730_template_dir(), ec);
                state.settings.cs2_video.folder_source_label.clear();
                state.save_settings();
            }
        }
        hover_tooltip("Pick a 730 folder (the CS2 userdata settings folder). A copy is taken "
                      "now and, on login, merged into userdata/<id>/730. Re-choose to update "
                      "the copy.");
        // Soft check: the snapshot should contain local/cfg if it is really a 730 folder.
        if (!state.settings.cs2_video.folder_source_label.empty()) {
            std::error_code vec;
            if (!std::filesystem::is_directory(
                    app::cs2_730_template_dir() / "local" / "cfg", vec)) {
                ImGui::TextColored(ImVec4(0.90F, 0.70F, 0.20F, 1.0F),
                                   "Note: no local/cfg inside; is this really a 730 folder?");
            }
        }
    }

    ImGui::Spacing();
    separator_text("Gamesense");
    {
        static std::string g_gamesense_err;
        const auto loader = app::gamesense_loader_path();
        ImGui::TextUnformatted("Loader:");
        ImGui::SameLine();
        if (loader) {
            ImGui::TextDisabled("%s", loader->filename().string().c_str());
        } else {
            ImGui::TextDisabled("none configured");
        }
        if (action_button(loader ? "Update loader..." : "Choose loader...",
                          ImVec2(160, 0))) {
            platform::file_dialog::Options opts;
            opts.parent = state.main_hwnd;
            opts.title = L"Choose gamesense loader";
            opts.filters = {{L"Executable (*.exe)", L"*.exe"},
                            {L"All files (*.*)", L"*.*"}};
            const auto res = platform::file_dialog::open_file(opts);
            if (res.ok) {
                g_gamesense_err.clear();
                if (!app::install_gamesense_loader(res.path, &g_gamesense_err)) {
                    SAM_LOG_ERROR("gamesense loader install failed: {}", g_gamesense_err);
                }
            }
        }
        if (loader) {
            ImGui::SameLine();
            if (action_button("Clear##gamesense-loader", ImVec2(80, 0))) {
                std::error_code ec;
                std::filesystem::remove(*loader, ec);
            }
        }
        if (!g_gamesense_err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", g_gamesense_err.c_str());
            ImGui::PopStyleColor();
        }
    }
    hover_tooltip("The loader .exe is copied into the app data folder (data\\gamesense). "
                  "Accounts set to \"Launch CS2 + gamesense\" run it after CS2 starts. "
                  "Pick again to update the loader.");

    ImGui::PopItemWidth();
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0.0F, kFooterGap));
    if (action_button("Save settings", ImVec2(160, 0))) {
        state.save_settings();
        ImGui::OpenPopup("Settings saved");
    }
    hover_tooltip("Persist settings to the config directory.");
    ImGui::SameLine();
    if (action_button("Open data folder", ImVec2(160, 0))) {
        open_folder(platform::data_dir());
    }
    hover_tooltip("Open the account manager data folder (vault, settings, logs) in Explorer.");

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
