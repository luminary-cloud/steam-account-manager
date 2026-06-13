#include "ui/widgets/bundle_dialogs.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <string_view>
#include <utility>

#include <imgui.h>

#include "core/crypto/secure_string.hpp"
#include "platform/file_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::widgets {

namespace {

constexpr const char* kExportPopupName = "Export bundle";
constexpr const char* kImportPopupName = "Import bundle";

void wipe(std::array<char, kBundlePassphraseBufLen>& buf) {
    crypto::zero_buffer(buf.data(), buf.size());
}

std::string default_export_filename() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &now);
#else
    tm = *std::localtime(&now);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "steam-accounts-%Y-%m-%d.samvault", &tm);
    return buf;
}

void render_passphrase_input(const char* label,
                              std::array<char, kBundlePassphraseBufLen>& buf,
                              bool& show) {
    const ImGuiInputTextFlags flags = show
        ? ImGuiInputTextFlags_None
        : ImGuiInputTextFlags_Password;
    ImGui::InputText(label, buf.data(), buf.size(), flags);
}

}  // namespace

void request_export_bundle(ExportBundleState& state,
                            std::vector<std::string> ids) {
    wipe(state.passphrase_buf);
    wipe(state.confirm_buf);
    state.account_ids = std::move(ids);
    state.status.clear();
    state.done = false;
    state.show_passphrase = false;
    state.open = true;
    ImGui::OpenPopup(kExportPopupName);
}

void request_import_bundle(ImportBundleState& state,
                            std::filesystem::path path) {
    wipe(state.passphrase_buf);
    state.picked_path = std::move(path);
    state.error.clear();
    state.status.clear();
    state.have_preview = false;
    state.preview_vault = {};
    state.preview_report = {};
    state.done = false;
    state.show_passphrase = false;
    state.open = true;
    ImGui::OpenPopup(kImportPopupName);
}

void draw_export_bundle_modal(app::AppState& app, ExportBundleState& state) {
    if (!state.open) return;

    ImGui::SetNextWindowSize(ImVec2(460.0F, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool visible = ImGui::BeginPopupModal(
        kExportPopupName, &state.open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!visible) {
        ImGui::PopStyleVar(2);
        // ImGui closed the popup (X / Escape): wipe sensitive buffers so a
        // typed passphrase doesn't linger in memory.
        wipe(state.passphrase_buf);
        wipe(state.confirm_buf);
        state.status.clear();
        state.account_ids.clear();
        state.done = false;
        return;
    }

    if (state.done) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::TextWrapped("%s", state.status.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (action_button("Close", ImVec2(100, 0))) {
            wipe(state.passphrase_buf);
            wipe(state.confirm_buf);
            state.open = false;
            state.done = false;
            state.status.clear();
            state.account_ids.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        ImGui::PopStyleVar(2);
        return;
    }

    const std::size_t n = state.account_ids.empty()
        ? app.vault.accounts.size()
        : state.account_ids.size();
    ImGui::TextWrapped("Export %zu account%s as an encrypted bundle. The bundle "
                       "is locked under a passphrase you choose here. It does "
                       "not need to match your master password.",
                       n, n == 1 ? "" : "s");
    ImGui::Spacing();

    ImGui::PushItemWidth(-100.0F);
    render_passphrase_input("Passphrase", state.passphrase_buf, state.show_passphrase);
    render_passphrase_input("Confirm",    state.confirm_buf,    state.show_passphrase);
    ImGui::PopItemWidth();
    ImGui::Checkbox("Show", &state.show_passphrase);

    if (!state.status.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("%s", state.status.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    const std::string_view pass(state.passphrase_buf.data());
    const std::string_view conf(state.confirm_buf.data());
    const bool valid = !pass.empty() && pass == conf;

    ImGui::BeginDisabled(!valid);
    if (action_button("Save...", ImVec2(100, 0))) {
        platform::file_dialog::Options opts;
        opts.parent = app.main_hwnd;
        opts.title = L"Export bundle";
        opts.default_filename = to_wide(default_export_filename());
        opts.filters = {
            {L"Steam Account Manager bundle (*.samvault)", L"*.samvault"},
            {L"All files (*.*)", L"*.*"},
        };
        opts.default_extension = L"samvault";
        auto picked = platform::file_dialog::save_file(opts);
        if (picked.ok) {
            try {
                const core::Vault subset = state.account_ids.empty()
                    ? app.vault
                    : core::store::subset_vault(app.vault, state.account_ids);
                auto pw = crypto::make_secure(pass);
                core::store::save_bundle(picked.path, subset, pw);
                crypto::zero_buffer(pw.data(), pw.size());
                state.status = "Exported " + std::to_string(subset.accounts.size()) +
                               " account(s) to " + picked.path.string();
                state.done = true;
            } catch (const std::exception& ex) {
                state.status = std::string("Export failed: ") + ex.what();
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (action_button("Cancel", ImVec2(100, 0))) {
        wipe(state.passphrase_buf);
        wipe(state.confirm_buf);
        state.open = false;
        state.status.clear();
        state.account_ids.clear();
        ImGui::CloseCurrentPopup();
    }

    if (!valid && !pass.empty() && pass != conf) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextUnformatted("Passphrases do not match.");
        ImGui::PopStyleColor();
    }

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

void draw_import_bundle_modal(app::AppState& app, ImportBundleState& state) {
    if (!state.open) return;

    ImGui::SetNextWindowSize(ImVec2(480.0F, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool visible = ImGui::BeginPopupModal(
        kImportPopupName, &state.open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!visible) {
        ImGui::PopStyleVar(2);
        // External close: wipe the passphrase buffer.
        wipe(state.passphrase_buf);
        state.error.clear();
        state.status.clear();
        state.have_preview = false;
        state.preview_vault = {};
        state.preview_report = {};
        state.picked_path.clear();
        state.done = false;
        return;
    }

    auto close_modal = [&] {
        wipe(state.passphrase_buf);
        state.open = false;
        state.error.clear();
        state.status.clear();
        state.have_preview = false;
        state.preview_vault = {};
        state.preview_report = {};
        state.picked_path.clear();
        state.done = false;
        ImGui::CloseCurrentPopup();
    };

    if (state.done) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::TextWrapped("%s", state.status.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (action_button("Close", ImVec2(100, 0))) {
            close_modal();
        }
        ImGui::EndPopup();
        ImGui::PopStyleVar(2);
        return;
    }

    ImGui::TextDisabled("File:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", state.picked_path.string().c_str());
    ImGui::Spacing();

    if (state.have_preview) {
        ImGui::TextWrapped("This bundle contains %zu account(s):",
                           state.preview_vault.accounts.size());
        ImGui::BulletText("%zu new account(s) will be added",
                          state.preview_report.added);
        ImGui::BulletText("%zu existing account(s) will be overwritten",
                          state.preview_report.overwritten);
        if (!state.preview_report.overwritten_logins.empty()) {
            ImGui::Indent(20.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            const auto& logins = state.preview_report.overwritten_logins;
            const std::size_t shown = std::min<std::size_t>(5, logins.size());
            for (std::size_t i = 0; i < shown; ++i) {
                ImGui::BulletText("%s", logins[i].c_str());
            }
            if (logins.size() > shown) {
                ImGui::BulletText("... and %zu more",
                                  logins.size() - shown);
            }
            ImGui::PopStyleColor();
            ImGui::Unindent(20.0F);
        }
        if (state.preview_report.tags_added > 0) {
            ImGui::BulletText("%zu new tag(s) will be added",
                              state.preview_report.tags_added);
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("Overwriting replaces the existing account in place. "
                           "The change is saved to disk immediately on confirm.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (action_button("Import", ImVec2(100, 0))) {
            try {
                core::store::merge_into(app.vault,
                                        std::move(state.preview_vault));
                app.vault_dirty = true;
                app.flush_pending_save();
                state.status = "Imported " +
                    std::to_string(state.preview_report.added +
                                   state.preview_report.overwritten) +
                    " account(s).";
                state.done = true;
            } catch (const std::exception& ex) {
                state.error = std::string("Import failed: ") + ex.what();
            }
        }
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) {
            close_modal();
        }
    } else {
        ImGui::TextWrapped("Enter the passphrase the bundle was exported under.");
        ImGui::Spacing();
        ImGui::PushItemWidth(-100.0F);
        render_passphrase_input("Passphrase", state.passphrase_buf,
                                state.show_passphrase);
        ImGui::PopItemWidth();
        ImGui::Checkbox("Show", &state.show_passphrase);

        if (!state.error.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", state.error.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        const std::string_view pass(state.passphrase_buf.data());
        ImGui::BeginDisabled(pass.empty());
        if (action_button("Unlock", ImVec2(100, 0))) {
            state.error.clear();
            try {
                auto pw = crypto::make_secure(pass);
                core::Vault imported =
                    core::store::load_bundle(state.picked_path, pw);
                crypto::zero_buffer(pw.data(), pw.size());
                state.preview_report =
                    core::store::preview_merge(app.vault, imported);
                state.preview_vault = std::move(imported);
                state.have_preview = true;
            } catch (const core::store::WrongPassword&) {
                state.error = "Incorrect passphrase.";
            } catch (const core::store::CorruptVault& ex) {
                state.error = std::string("Bundle is corrupt: ") + ex.what();
            } catch (const core::store::UnsupportedVersion& ex) {
                state.error = std::string("Unsupported bundle version: ") +
                              ex.what();
            } catch (const std::exception& ex) {
                state.error = std::string("Read failed: ") + ex.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) {
            close_modal();
        }
    }

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

}  // namespace sam::ui::widgets
