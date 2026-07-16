#include "ui/screens/settings_sections.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "app/app_paths.hpp"
#include "app/gamesense_loader.hpp"
#include "app/luminary_loader.hpp"
#include "app/job_pump.hpp"
#include "app/vault_registry.hpp"
#include "core/account_store/store.hpp"
#include "core/cs2_config/video_config.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/http/client.hpp"
#include "core/log.hpp"
#include "platform/dpapi.hpp"
#include "platform/file_dialog.hpp"
#include "platform/fs.hpp"
#include "platform/paths.hpp"
#include "ui/screens/unlock_screen.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/master_pw_field.hpp"
#include "ui/widgets/rail_nav.hpp"

namespace sam::ui::screens {
namespace settings_detail {

bool write_master_pw_cache(app::AppState& state) {
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
}

void draw_proxy_section(app::AppState& state) {
    separator_text("Proxy");

    // Shared by both Test buttons: forces the given proxy regardless of mode, reports exit IP.
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
}

void draw_authenticator_section(app::AppState& state) {
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
}

void draw_confirmations_section(app::AppState& state) {
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
}

void draw_cs2_config_section(app::AppState& state) {
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
                // Keep our own copy so it survives the original being moved or deleted.
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
                // Snapshot into app data so it survives the original being moved or deleted.
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
        // A real 730 folder should contain local/cfg.
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
    char lo_buf[256];
    std::snprintf(lo_buf, sizeof(lo_buf), "%s",
                  state.settings.cs2_video.launch_options.c_str());
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText("CS2 launch options", lo_buf, sizeof(lo_buf))) {
        state.settings.cs2_video.launch_options = lo_buf;
        state.save_settings();
    }
    hover_tooltip("Written to appid 730's LaunchOptions for the account you launch (e.g. "
                  "-novid -tickrate 128 +fps_max 400). Applied while Steam is restarting so "
                  "Steam won't overwrite it. Leave empty to keep Steam's current options.");
}

void draw_gamesense_section(app::AppState& state) {
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
}

void draw_luminary_section(app::AppState& state) {
    separator_text("Luminary");
    {
        static std::string g_luminary_err;
        const auto loader = app::luminary_loader_path();
        ImGui::TextUnformatted("Loader:");
        ImGui::SameLine();
        if (loader) {
            ImGui::TextDisabled("%s", loader->filename().string().c_str());
        } else {
            ImGui::TextDisabled("none configured");
        }
        if (action_button(loader ? "Update loader...##lum" : "Choose loader...##lum",
                          ImVec2(160, 0))) {
            platform::file_dialog::Options opts;
            opts.parent = state.main_hwnd;
            opts.title = L"Choose luminary loader";
            opts.filters = {{L"Executable (*.exe)", L"*.exe"},
                            {L"All files (*.*)", L"*.*"}};
            const auto res = platform::file_dialog::open_file(opts);
            if (res.ok) {
                g_luminary_err.clear();
                if (!app::install_luminary_loader(res.path, &g_luminary_err)) {
                    SAM_LOG_ERROR("luminary loader install failed: {}", g_luminary_err);
                }
            }
        }
        if (loader) {
            ImGui::SameLine();
            if (action_button("Clear##luminary-loader", ImVec2(80, 0))) {
                std::error_code ec;
                std::filesystem::remove(*loader, ec);
            }
        }
        if (!g_luminary_err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", g_luminary_err.c_str());
            ImGui::PopStyleColor();
        }
    }
    hover_tooltip("The loader .exe is copied into the app data folder (data\\luminary). "
                  "Accounts set to \"Launch CS2 + luminary\" run it with --auto --game=cs2 "
                  "after CS2 starts. Pick again to update the loader.");
}

void draw_storage_section(app::AppState& state) {
    separator_text("Storage");
    {
        constexpr ImVec4 kWarn(0.90F, 0.70F, 0.20F, 1.0F);
        static std::string s_storage_err;
        static std::filesystem::path s_pending_dir;  // target awaiting confirmation
        static bool s_pending_is_default = false;
        static bool s_restart_pending = false;       // a change applied this session

        ImGui::TextUnformatted("Data folder:");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", platform::data_dir().string().c_str());

        if (const auto bad = platform::custom_data_dir_unavailable()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
            ImGui::TextWrapped("Chosen folder \"%s\" was unavailable; using the default location.",
                               bad->string().c_str());
            ImGui::PopStyleColor();
        }

        if (s_restart_pending) {
            ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
            ImGui::TextWrapped("Data folder updated. Restart the app to start using the new "
                               "location; the old folder is removed on the next launch.");
            ImGui::PopStyleColor();
            if (action_button("Open data folder", ImVec2(160, 0))) {
                open_folder(platform::data_dir());
            }
        } else {
            if (action_button("Change folder...", ImVec2(160, 0))) {
                platform::file_dialog::Options opts;
                opts.parent = state.main_hwnd;
                opts.title = L"Choose a folder for the account manager data";
                const auto res = platform::file_dialog::pick_folder(opts);
                if (res.ok) {
                    s_storage_err.clear();
                    s_pending_dir = res.path;
                    s_pending_is_default = false;
                    ImGui::OpenPopup("Move data folder");
                }
            }
            hover_tooltip("Copy the vault, settings, and logs to another folder (e.g. a second "
                          "drive or USB stick). Takes effect after a restart.");

            if (platform::using_custom_data_dir()) {
                ImGui::SameLine();
                if (action_button("Reset to default", ImVec2(160, 0))) {
                    s_storage_err.clear();
                    s_pending_dir = platform::default_data_dir();
                    s_pending_is_default = true;
                    ImGui::OpenPopup("Move data folder");
                }
                hover_tooltip("Move the data back to the default location under "
                              "%LOCALAPPDATA%.");
            }

            ImGui::SameLine();
            if (action_button("Open data folder", ImVec2(160, 0))) {
                open_folder(platform::data_dir());
            }
            hover_tooltip("Open the data folder (vault, settings, logs) in Explorer.");
        }

        if (!s_storage_err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", s_storage_err.c_str());
            ImGui::PopStyleColor();
        }

        if (begin_styled_modal("Move data folder")) {
            ImGui::TextWrapped(s_pending_is_default
                ? "Move all data back to the default location:"
                : "Copy all data to:");
            ImGui::Spacing();
            ImGui::TextDisabled("%s", s_pending_dir.string().c_str());
            ImGui::Spacing();
            std::error_code vec;
            if (std::filesystem::exists(s_pending_dir / "vault.bin", vec)) {
                ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
                ImGui::TextWrapped("That folder already has a vault.bin; it will be overwritten.");
                ImGui::PopStyleColor();
            }
            ImGui::TextWrapped("The current folder is removed on the next launch, and the app "
                               "must be restarted to apply the change.");
            ImGui::Spacing();
            if (action_button("Copy and apply", ImVec2(140, 0))) {
                std::string err;
                if (platform::relocate_data_dir(s_pending_dir, &err)) {
                    if (s_pending_is_default) {
                        platform::clear_custom_data_dir(nullptr);  // nullptr = default location
                    }
                    s_restart_pending = true;
                } else {
                    s_storage_err = err;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (action_button("Cancel", ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
            }
            end_styled_modal();
        }
    }
}

namespace {

std::uint32_t vaults_pack_rgba(const float c[4]) {
    auto to_byte = [](float f) {
        f = std::clamp(f, 0.0F, 1.0F);
        return static_cast<std::uint32_t>(f * 255.0F + 0.5F);
    };
    return (to_byte(c[0]) << 24) | (to_byte(c[1]) << 16) |
           (to_byte(c[2]) << 8) | to_byte(c[3]);
}

void vaults_unpack_rgba(std::uint32_t rgba, float out[4]) {
    out[0] = ((rgba >> 24) & 0xFF) / 255.0F;
    out[1] = ((rgba >> 16) & 0xFF) / 255.0F;
    out[2] = ((rgba >> 8) & 0xFF) / 255.0F;
    out[3] = (rgba & 0xFF) / 255.0F;
}

}  // namespace

void draw_vaults_section(app::AppState& state) {
    separator_text("Vaults");
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
    ImGui::TextWrapped("Each vault holds its own accounts and its own settings. A few "
                       "options, marked \"(global)\", apply to every vault. Switching "
                       "vaults reopens the app.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    auto& reg = state.vault_registry;
    const std::string active = platform::active_vault_id();

    // Deferred actions so we don't mutate the vault list mid-iteration.
    std::string switch_to, delete_id;

    std::vector<app::VaultInfo*> items;
    items.reserve(reg.vaults.size());
    for (auto& v : reg.vaults) items.push_back(&v);
    std::sort(items.begin(), items.end(), [](auto* a, auto* b) {
        if (a->order != b->order) return a->order < b->order;
        return a->name < b->name;
    });

    for (auto* vp : items) {
        app::VaultInfo& v = *vp;
        const bool is_active = v.id == active;
        ImGui::PushID(v.id.c_str());

        // Colour swatch (commit on release so we don't rewrite the registry per frame).
        float col[4];
        vaults_unpack_rgba(v.color_rgba, col);
        if (ImGui::ColorEdit4("##color", col,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            v.color_rgba = vaults_pack_rgba(col);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) app::save_registry(reg);
        ImGui::SameLine();

        // Name (commit on deactivate).
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "%s", v.name.c_str());
        ImGui::SetNextItemWidth(200.0F);
        if (ImGui::InputText("##name", name_buf, sizeof(name_buf))) v.name = name_buf;
        if (ImGui::IsItemDeactivatedAfterEdit()) app::rename_vault(reg, v.id, v.name);

        ImGui::SameLine();
        if (is_active) {
            ImGui::TextColored(theme::success(), "current");
        } else if (action_button("Switch to")) {
            switch_to = v.id;
        }

        // Auto-open toggle (mutually exclusive across vaults).
        bool auto_open = reg.auto_open_id == v.id;
        if (ImGui::Checkbox("Auto-open at startup", &auto_open)) {
            app::set_auto_open(reg, auto_open ? v.id : std::string{});
        }
        hover_tooltip("Open this vault automatically on launch (skipping the picker) "
                      "when it can unlock silently. Only one vault can auto-open.");

        ImGui::SameLine();
        if (action_button("Set icon...")) {
            platform::file_dialog::Options opts;
            opts.parent = state.main_hwnd;
            opts.title = L"Choose a vault icon image";
            opts.filters = {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif"}};
            const auto res = platform::file_dialog::open_file(opts);
            if (res.ok) {
                std::string err;
                if (!app::set_vault_icon(reg, v.id, res.path, &err))
                    SAM_LOG_WARN("vault icon set failed: {}", err);
            }
        }
        if (!v.icon_file.empty()) {
            ImGui::SameLine();
            if (action_button("Remove icon")) app::clear_vault_icon(reg, v.id);
        }

        ImGui::SameLine();
        const bool can_delete = reg.vaults.size() > 1 && !is_active;
        ImGui::BeginDisabled(!can_delete);
        if (action_button("Delete")) delete_id = v.id;
        ImGui::EndDisabled();
        if (!can_delete) {
            hover_tooltip(is_active ? "Switch to another vault before deleting this one."
                                    : "You must keep at least one vault.");
        }

        ImGui::Spacing();
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (action_button("Create vault...")) ImGui::OpenPopup("Create vault");

    // ---- Create vault modal (creates the file + entry; does not switch to it) ----
    static std::string c_name, c_pw, c_pw_confirm, c_err;
    if (begin_styled_modal("Create vault")) {
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(320.0F);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", c_name.c_str());
        if (ImGui::InputText("##c_name", buf, sizeof(buf))) c_name = buf;

        ImGui::Spacing();
        ImGui::TextUnformatted("Master password");
        widgets::draw_password_field("##c_pw", c_pw, true, 320.0F);
        ImGui::TextUnformatted("Confirm");
        widgets::draw_password_field("##c_pw_confirm", c_pw_confirm, false, 320.0F);

        if (!c_err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", c_err.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        const bool ok = !c_name.empty() && !c_pw.empty() && c_pw == c_pw_confirm;
        ImGui::BeginDisabled(!ok);
        if (action_button("Create", ImVec2(120, 0))) {
            try {
                const auto id = app::new_vault_id();
                std::error_code ec;
                std::filesystem::create_directories(app::vault_dir_for(id), ec);
                sam::core::store::create_new_vault(
                    app::vault_dir_for(id) / L"vault.bin",
                    sam::crypto::make_secure(c_pw));
                app::add_vault_entry(reg, id, c_name, 0x4F8EF7FFu);
                c_name.clear();
                c_pw.clear();
                c_pw_confirm.clear();
                c_err.clear();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& ex) {
                c_err = ex.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(90, 0))) {
            c_pw.clear();
            c_pw_confirm.clear();
            c_err.clear();
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }

    // ---- Delete confirmation ----
    static std::string s_delete_pending;
    if (!delete_id.empty()) {
        s_delete_pending = delete_id;
        ImGui::OpenPopup("Delete vault");
    }
    if (begin_styled_modal("Delete vault")) {
        const auto* v = reg.find(s_delete_pending);
        ImGui::TextWrapped("Permanently delete vault \"%s\" and all its accounts? "
                           "This cannot be undone.",
                           v ? v->name.c_str() : "");
        ImGui::Spacing();
        if (action_button("Delete", ImVec2(120, 0))) {
            std::string err;
            if (!app::remove_vault(reg, s_delete_pending, &err))
                SAM_LOG_WARN("vault delete failed: {}", err);
            s_delete_pending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(90, 0))) {
            s_delete_pending.clear();
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }

    if (!switch_to.empty()) widgets::request_vault_switch(state, switch_to);
}

}  // namespace settings_detail
}  // namespace sam::ui::screens
