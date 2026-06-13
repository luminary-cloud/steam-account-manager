#include "ui/screens/unlock_screen.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <string>

#include <windows.h>

#include <imgui.h>

#include "app/app_paths.hpp"
#include "core/account_store/store.hpp"
#include "core/log.hpp"
#include "platform/dpapi.hpp"
#include "platform/file_dialog.hpp"
#include "platform/fs.hpp"
#include "platform/paths.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/master_pw_field.hpp"

namespace sam::ui::screens {

namespace {

enum class Mode { Existing, Create };

void try_unlock(app::AppState& state, const std::string& pw, std::string& error) {
    try {
        state.vault = sam::core::store::load_vault(app::vault_path(),
                                                    sam::crypto::make_secure(pw));
        state.master_password = sam::crypto::make_secure(pw);
        state.unlocked = true;
        state.last_interaction = std::chrono::steady_clock::now();
        state.current_screen = app::Screen::Accounts;
        // If the user has the auto-unlock setting enabled, refresh the DPAPI
        // cache now that we know the password works.
        if (state.settings.remember_master_password) {
            try {
                std::span<const std::uint8_t> pw_bytes{
                    reinterpret_cast<const std::uint8_t*>(pw.data()), pw.size()};
                auto wrapped = sam::platform::dpapi::protect(pw_bytes);
                sam::platform::atomic_write_file(app::master_pw_cache_path(), wrapped);
            } catch (const std::exception& ex) {
                SAM_LOG_WARN("auto-unlock cache write failed: {}", ex.what());
            }
        }
        if (state.settings.refresh_on_launch) state.refresh_account_data();
        // One-time automatic fill of external funds for accounts that don't have
        // a figure yet (skipped when the feature is off); no-op once every
        // eligible account has been fetched.
        if (state.settings.info.show_external_funds) {
            state.refresh_all_spend(/*only_missing=*/true);
        }
        error.clear();
    } catch (const sam::core::store::WrongPassword&) {
        error = "Wrong master password.";
    } catch (const sam::core::store::CorruptVault& ex) {
        error = std::string{"Vault is corrupt: "} + ex.what();
    } catch (const std::exception& ex) {
        error = ex.what();
    }
}

void create_vault(app::AppState& state, const std::string& pw, std::string& error) {
    try {
        sam::core::store::create_new_vault(app::vault_path(),
                                           sam::crypto::make_secure(pw));
        state.vault = {};
        state.master_password = sam::crypto::make_secure(pw);
        state.unlocked = true;
        state.last_interaction = std::chrono::steady_clock::now();
        state.current_screen = app::Screen::Accounts;
        error.clear();
    } catch (const std::exception& ex) {
        error = ex.what();
    }
}

}  // namespace

void draw_unlock(app::AppState& state) {
    static std::string password;
    static std::string password_confirm;
    static std::string error_message;
    static Mode mode = sam::core::store::vault_exists(app::vault_path())
        ? Mode::Existing
        : Mode::Create;

    const auto& vp = *ImGui::GetMainViewport();
    const ImVec2 panel_size{420.0F, 380.0F};
    const ImVec2 center{vp.WorkPos.x + vp.WorkSize.x / 2.0F - panel_size.x / 2.0F,
                        vp.WorkPos.y + vp.WorkSize.y / 2.0F - panel_size.y / 2.0F};
    ImGui::SetCursorPos(ImVec2(center.x - vp.WorkPos.x, center.y - vp.WorkPos.y));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel());
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0F, 1.0F, 1.0F, 0.06F));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::BeginChild("##unlock", panel_size, ImGuiChildFlags_Borders);

    ImGui::Spacing();
    ImGui::TextUnformatted(mode == Mode::Existing ? "Unlock your vault" : "Create a new vault");

    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
    ImGui::TextWrapped(mode == Mode::Existing
        ? "Enter your master password to unlock the encrypted account database."
        : "Choose a master password. Lose it and your accounts cannot be recovered.");
    ImGui::PopStyleColor();

    ImGui::Spacing();

    ImGui::TextUnformatted("Password");
    widgets::draw_password_field("##unlock_pw", password,
                                  mode == Mode::Create, 310.0F);
    if (mode == Mode::Create) {
        ImGui::TextUnformatted("Confirm");
        widgets::draw_password_field("##unlock_confirm", password_confirm, false, 310.0F);
    }

    ImGui::Spacing();

    const bool can_submit =
        !password.empty() &&
        (mode != Mode::Create || password == password_confirm);

    ImGui::BeginDisabled(!can_submit);
    if (action_button(mode == Mode::Existing ? "Unlock" : "Create", ImVec2(120, 0))) {
        if (mode == Mode::Existing) {
            try_unlock(state, password, error_message);
        } else {
            create_vault(state, password, error_message);
        }
        if (state.unlocked) {
            password.clear();
            password_confirm.clear();
        }
    }
    ImGui::EndDisabled();

    if (mode == Mode::Create && !password_confirm.empty() && password != password_confirm) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextUnformatted("Passwords don't match");
        ImGui::PopStyleColor();
    }

    if (!error_message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("%s", error_message.c_str());
        ImGui::PopStyleColor();
    }

    if (mode == Mode::Existing) {
        ImGui::Spacing();
        if (action_button("I want to create a new vault instead")) {
            mode = Mode::Create;
            error_message.clear();
        }
    }

    // Recovery for an existing user whose data folder lives elsewhere (another
    // drive, a USB stick, or a fresh Windows install where the saved location
    // was lost). Point the app at that folder so it doesn't start a blank vault.
    static std::filesystem::path located_dir;
    static std::string locate_error;
    ImGui::Spacing();
    if (action_button("Locate an existing data folder...")) {
        platform::file_dialog::Options opts;
        opts.parent = state.main_hwnd;
        opts.title = L"Select your steam-account-manager data folder";
        const auto res = platform::file_dialog::pick_folder(opts);
        if (res.ok) {
            locate_error.clear();
            if (sam::core::store::vault_exists(res.path / "vault.bin")) {
                located_dir = res.path;
                ImGui::OpenPopup("Use data folder");
            } else {
                locate_error = "No vault.bin in that folder. Pick the folder that "
                               "holds your existing vault.";
            }
        }
    }
    hover_tooltip("Already have a data folder from another drive or a previous "
                  "install? Point the app at it (it must contain vault.bin).");
    if (!locate_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("%s", locate_error.c_str());
        ImGui::PopStyleColor();
    }

    if (begin_styled_modal("Use data folder")) {
        ImGui::TextWrapped("Load your accounts from:");
        ImGui::Spacing();
        ImGui::TextDisabled("%s", located_dir.string().c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("The app will close so it can reopen using this folder.");
        ImGui::Spacing();
        if (action_button("Set and close", ImVec2(140, 0))) {
            std::string err;
            if (platform::set_custom_data_dir(located_dir, &err)) {
                ImGui::CloseCurrentPopup();
                PostMessageW(state.main_hwnd, WM_CLOSE, 0, 0);
            } else {
                locate_error = err;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

}  // namespace sam::ui::screens
