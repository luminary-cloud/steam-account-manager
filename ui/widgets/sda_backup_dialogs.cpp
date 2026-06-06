#include "ui/widgets/sda_backup_dialogs.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/crypto/secure_string.hpp"
#include "core/sda/mafile_export.hpp"
#include "platform/clipboard.hpp"
#include "platform/file_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/redacted_text.hpp"

namespace sam::ui::widgets {

namespace {

constexpr const char* kBackupPopupName = "Backup authenticator";
constexpr const char* kExportPopupName = "Export maFiles";
constexpr const char* kSecretsPopupName = "Show secrets";

void wipe(std::array<char, kSdaPassphraseBufLen>& buf) {
    crypto::zero_buffer(buf.data(), buf.size());
}

void render_passphrase_input(const char* label,
                              std::array<char, kSdaPassphraseBufLen>& buf,
                              bool& show) {
    const ImGuiInputTextFlags flags = show
        ? ImGuiInputTextFlags_None
        : ImGuiInputTextFlags_Password;
    ImGui::InputText(label, buf.data(), buf.size(), flags);
}

std::string sanitize_filename(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == '/' || u == '\\' || u == ':' || u == '*' ||
            u == '?' || u == '"' || u == '<' || u == '>' || u == '|') {
            c = '_';
        }
    }
    if (s.empty()) s = "account";
    return s;
}

}  // namespace

void request_backup(BackupDialogState& state, std::string account_id) {
    state.account_id = std::move(account_id);
    state.synthesized_uri.clear();
    state.open = true;
    ImGui::OpenPopup(kBackupPopupName);
}

void request_export_mafiles(ExportMafilesState& state) {
    wipe(state.passphrase_buf);
    wipe(state.confirm_buf);
    state.selected_ids.clear();
    state.show_passphrase = false;
    state.status.clear();
    state.written = 0;
    state.skipped = 0;
    state.open = true;
    ImGui::OpenPopup(kExportPopupName);
}

void request_show_secrets(ShowSecretsState& state) {
    state.open = true;
    ImGui::OpenPopup(kSecretsPopupName);
}

void draw_backup_modal(app::AppState& app, BackupDialogState& state) {
    if (!state.open) return;
    ImGui::SetNextWindowSize(ImVec2(480.0F, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool visible = ImGui::BeginPopupModal(
        kBackupPopupName, &state.open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!visible) {
        ImGui::PopStyleVar(2);
        state.account_id.clear();
        state.synthesized_uri.clear();
        return;
    }

    auto* a = app.find_account(state.account_id);
    if (!a || !a->sda.has_value()) {
        ImGui::TextWrapped("This account no longer has Steam Guard attached.");
        ImGui::Spacing();
        if (action_button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        ImGui::PopStyleVar(2);
        return;
    }

    const std::string& stored_uri = a->sda->uri;
    const std::string& effective_uri = !stored_uri.empty() ? stored_uri : state.synthesized_uri;

    ImGui::TextWrapped("Copy this URI into any TOTP app (Google Authenticator, "
                       "1Password, Aegis, etc.) to back up the second factor.");
    ImGui::Spacing();

    if (stored_uri.empty() && state.synthesized_uri.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("This account has no stored otpauth URI. Older imports often "
                           "omit it. You can rebuild one from the stored shared_secret.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (action_button("Rebuild URI from shared_secret")) {
            state.synthesized_uri = sda::synthesize_otpauth_uri(
                a->login, a->sda->shared_secret);
        }
    } else {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextUnformatted(effective_uri.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (action_button("Copy URI")) {
            platform::clipboard::set_text(effective_uri);
        }
        if (stored_uri.empty()) {
            ImGui::SameLine();
            if (action_button("Save to vault")) {
                a->sda->uri = effective_uri;
                app.vault_dirty = true;
                app.save_vault_if_dirty();
            }
        }
    }

    ImGui::Spacing();
    if (action_button("Close")) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

void draw_export_mafiles_modal(app::AppState& app, ExportMafilesState& state) {
    if (!state.open) return;

    ImGui::SetNextWindowSize(ImVec2(560.0F, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool visible = ImGui::BeginPopupModal(
        kExportPopupName, &state.open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!visible) {
        ImGui::PopStyleVar(2);
        wipe(state.passphrase_buf);
        wipe(state.confirm_buf);
        state.selected_ids.clear();
        state.status.clear();
        state.written = 0;
        state.skipped = 0;
        return;
    }

    ImGui::TextWrapped("Pick accounts to export. Each becomes one encrypted .maFile "
                       "in a folder you choose. The passphrase is required again on "
                       "import.");

    ImGui::Spacing();
    ImGui::SeparatorText("Accounts");

    int n_with_sda = 0;
    for (const auto& a : app.vault.accounts) if (a.sda.has_value()) ++n_with_sda;

    if (n_with_sda == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextWrapped("No accounts in the vault have Steam Guard attached.");
        ImGui::PopStyleColor();
    } else {
        const float list_h = std::min(220.0F, 22.0F * static_cast<float>(n_with_sda + 1));
        ImGui::BeginChild("##acc-list", ImVec2(0, list_h), ImGuiChildFlags_Borders);
        for (const auto& a : app.vault.accounts) {
            if (!a.sda.has_value()) continue;
            bool checked = state.selected_ids.count(a.id) != 0;
            std::string label = !a.web.persona_name.empty()
                ? a.web.persona_name + "  " + a.login
                : a.login;
            label += "##" + a.id;
            if (ImGui::Checkbox(label.c_str(), &checked)) {
                if (checked) state.selected_ids.insert(a.id);
                else         state.selected_ids.erase(a.id);
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (ImGui::SmallButton("Select all")) {
            for (const auto& a : app.vault.accounts) {
                if (a.sda.has_value()) state.selected_ids.insert(a.id);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Select none")) state.selected_ids.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu selected", state.selected_ids.size());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Passphrase");
    render_passphrase_input("Passphrase", state.passphrase_buf, state.show_passphrase);
    render_passphrase_input("Confirm##cp", state.confirm_buf, state.show_passphrase);
    ImGui::Checkbox("Show passphrase", &state.show_passphrase);

    const bool match = std::strcmp(state.passphrase_buf.data(),
                                    state.confirm_buf.data()) == 0;
    const bool nonempty = state.passphrase_buf[0] != '\0';
    const bool ready = match && nonempty && !state.selected_ids.empty();

    if (!nonempty) {
        ImGui::TextDisabled("Passphrase required.");
    } else if (!match) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextUnformatted("Passphrase confirmation does not match.");
        ImGui::PopStyleColor();
    }

    if (!state.status.empty()) {
        ImGui::Spacing();
        const ImVec4 col = (state.written > 0 && state.skipped == 0)
            ? theme::success() : theme::warning();
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s", state.status.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!ready);
    if (action_button("Export...", ImVec2(140, 0))) {
        platform::file_dialog::Options opts;
        opts.parent = app.main_hwnd;
        opts.title = L"Pick output folder";
        const auto picked = platform::file_dialog::pick_folder(opts);
        if (picked.ok) {
            crypto::SecureString pass;
            pass.assign(state.passphrase_buf.begin(),
                        state.passphrase_buf.begin() +
                            std::strlen(state.passphrase_buf.data()));

            int wrote = 0;
            int failed = 0;
            std::vector<std::string> errors;
            for (const auto& a : app.vault.accounts) {
                if (!state.selected_ids.count(a.id)) continue;
                if (!a.sda.has_value()) { ++failed; continue; }
                const std::string fname = sanitize_filename(a.login) + ".maFile";
                const auto path = picked.path / fname;
                std::string err;
                if (sda::encrypt_and_write_mafile(path, a, pass, &err)) {
                    ++wrote;
                } else {
                    ++failed;
                    if (errors.size() < 3) errors.push_back(a.login + ": " + err);
                }
            }
            state.written = wrote;
            state.skipped = failed;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Wrote %d file(s); %d failed.",
                          wrote, failed);
            state.status = buf;
            for (const auto& e : errors) state.status += "\n - " + e;

            crypto::zero_buffer(pass.data(), pass.size());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (action_button("Close", ImVec2(100, 0))) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

void draw_show_secrets_modal(app::AppState& app, ShowSecretsState& state) {
    if (!state.open) return;

    ImGui::SetNextWindowSize(ImVec2(720.0F, 480.0F), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    const bool visible = ImGui::BeginPopupModal(
        kSecretsPopupName, &state.open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!visible) { ImGui::PopStyleVar(2); return; }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
    ImGui::TextWrapped("These secrets unlock the account. Anyone who sees them can "
                       "generate codes and approve confirmations. Treat them like a "
                       "password.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    ImGui::BeginChild("##secrets-body", ImVec2(0, -36.0F));

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_SizingStretchProp |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##secrets", 3, flags)) {
        ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthFixed, 160.0F);
        ImGui::TableSetupColumn("Field",   ImGuiTableColumnFlags_WidthFixed, 140.0F);
        ImGui::TableSetupColumn("Value",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto row = [&](const std::string& acc_label, const char* field,
                       const std::string& value) {
            if (value.empty()) return;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(acc_label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(field);
            ImGui::TableNextColumn();
            ImGui::PushID(field);
            ImGui::PushID(acc_label.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::TextWrapped("%s", value.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                platform::clipboard::set_text(value);
            }
            ImGui::PopID();
            ImGui::PopID();
        };

        for (const auto& a : app.vault.accounts) {
            if (!a.sda.has_value()) continue;
            const std::string label = a.login;
            row(label, "shared_secret",   a.sda->shared_secret);
            row(label, "identity_secret", a.sda->identity_secret);
            row(label, "revocation_code", a.sda->revocation_code);
            row(label, "device_id",       a.sda->device_id);
            row(label, "serial_number",   a.sda->serial_number);
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();

    if (action_button("Close", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

}  // namespace sam::ui::widgets
