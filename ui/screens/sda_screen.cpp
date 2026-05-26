#include "ui/screens/sda_screen.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "core/sda/totp.hpp"
#include "core/time_aligner.hpp"
#include "platform/clipboard.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar.hpp"
#include "ui/widgets/code_display.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/sda_backup_dialogs.hpp"
#include "ui/widgets/sda_wizard_dialogs.hpp"
#include "ui/widgets/search_bar.hpp"

namespace sam::ui::screens {

namespace {

constexpr const char* kDot = " \xC2\xB7 ";

std::string account_label(app::AppState& state, const core::Account& a) {
    const std::string login = widgets::login_label(state, a);
    if (a.web.persona_name.empty()) return login;
    return a.web.persona_name + kDot + login;
}

std::string lowercase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void draw_account_picker(app::AppState& state) {
    static std::string g_picker_filter;
    widgets::draw_search_bar(g_picker_filter, 280.0F);

    auto* current = state.find_account(state.selected_account_id);
    const std::string preview = current
        ? account_label(state, *current)
        : std::string{"Select an account"};

    const std::string filter_lower = lowercase(g_picker_filter);

    if (ImGui::BeginCombo("##picker", preview.c_str())) {
        for (auto& a : state.vault.accounts) {
            if (!filter_lower.empty()) {
                const std::string hay = lowercase(account_label(state, a));
                if (hay.find(filter_lower) == std::string::npos) continue;
            }
            std::string label = account_label(state, a);
            if (!a.sda.has_value()) {
                label += "  (no SDA)";
            } else if (!a.sda->fully_enrolled) {
                label += "  (incomplete)";
            }
            label += "##" + a.id;
            if (ImGui::Selectable(label.c_str(), a.id == state.selected_account_id)) {
                state.selected_account_id = a.id;
            }
        }
        ImGui::EndCombo();
    }
}

void draw_account_header(app::AppState& state, const core::Account& a) {
    widgets::draw_avatar(a, 48.0F);
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::BeginGroup();

    const std::string login = widgets::login_label(state, a);
    const bool has_persona = !a.web.persona_name.empty();
    ImGui::TextUnformatted(has_persona ? a.web.persona_name.c_str() : login.c_str());

    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
    if (has_persona && a.steam_id_64 != 0) {
        ImGui::Text("%s%s%llu", login.c_str(), kDot,
                    static_cast<unsigned long long>(a.steam_id_64));
    } else if (has_persona) {
        ImGui::TextUnformatted(login.c_str());
    } else if (a.steam_id_64 != 0) {
        ImGui::Text("%llu", static_cast<unsigned long long>(a.steam_id_64));
    }
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

std::string trim_copy(const char* s) {
    std::string out(s);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front()))) {
        out.erase(out.begin());
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return out;
}

void draw_revocation_section(app::AppState& state, core::Account& a) {
    static std::unordered_map<std::string, std::int64_t> g_rcode_reveal_until_unix;

    ImGui::SeparatorText("Revocation code");
    ImGui::Spacing();

    if (!a.sda->revocation_code.empty()) {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto until = g_rcode_reveal_until_unix[a.id];
        const bool revealed = until > now;

        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        if (revealed) {
            ImGui::TextUnformatted(a.sda->revocation_code.c_str());
        } else {
            std::string masked = "R";
            masked.append(a.sda->revocation_code.size() > 1
                ? a.sda->revocation_code.size() - 1 : 5, '*');
            ImGui::TextUnformatted(masked.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (action_button(revealed ? "Hide" : "Reveal")) {
            g_rcode_reveal_until_unix[a.id] = revealed ? 0 : (now + 10);
        }
        ImGui::SameLine();
        if (action_button("Copy")) {
            platform::clipboard::set_text(a.sda->revocation_code);
        }
        hover_tooltip("Copy the revocation code to the clipboard.");
    } else {
        // Per-account input buffer. Reset when the user switches accounts so
        // a half-typed code doesn't leak into the next account view.
        static std::array<char, 32> buf{};
        static std::string buf_owner_id;
        if (buf_owner_id != a.id) {
            buf_owner_id = a.id;
            buf.fill(0);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("No revocation code stored for this account. Paste the R-code you "
                           "saved when you set up the authenticator.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(180.0F);
        ImGui::InputText("##rev-input", buf.data(), buf.size());
        ImGui::SameLine();

        const std::string trimmed = trim_copy(buf.data());
        ImGui::BeginDisabled(trimmed.empty());
        if (action_button("Save")) {
            a.sda->revocation_code = trimmed;
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            buf.fill(0);
        }
        ImGui::EndDisabled();
        hover_tooltip("Stores the revocation code in your encrypted vault. Steam cannot "
                      "regenerate it, only enter a code you saved yourself.");
    }

    ImGui::Spacing();
    ImGui::TextWrapped("Removing Steam Guard from an account triggers a 15-day trade and market hold. "
                       "Keep the revocation code somewhere safe - it cannot be regenerated.");
}

}  // namespace

void draw_time_sync_banner() {
    constexpr std::int64_t kStaleAfterSeconds = 2 * 3600;
    const bool synced = time_aligner::synced();
    const auto stale = time_aligner::seconds_since_last_success();
    if (synced && stale < kStaleAfterSeconds) return;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
    if (!synced) {
        ImGui::TextWrapped("Time sync with Steam hasn't completed yet. "
                           "Codes will be hidden until it does.");
    } else {
        const std::int64_t mins = stale / 60;
        if (mins >= 60) {
            ImGui::Text("Time sync is %lldh stale.",
                        static_cast<long long>(mins / 60));
        } else {
            ImGui::Text("Time sync is %lld min stale.",
                        static_cast<long long>(mins));
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (action_button("Sync now")) {
        time_aligner::sync_now_async();
    }
    ImGui::Spacing();
}

void draw_sda(app::AppState& state) {
    static widgets::BackupDialogState backup_state;
    static widgets::ExportMafilesState export_state;
    static widgets::ShowSecretsState   secrets_state;
    static widgets::AddSdaDialogState  add_sda_state;
    static widgets::RemoveSdaDialogState remove_sda_state;

    ImGui::TextUnformatted("Authenticator");
    ImGui::SameLine();
    {
        auto* current = state.find_account(state.selected_account_id);
        const bool can_backup = current && current->sda.has_value();
        ImGui::BeginDisabled(!can_backup);
        if (action_button("Backup...")) {
            widgets::request_backup(backup_state, current->id);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (action_button("Export maFiles...")) {
            widgets::request_export_mafiles(export_state);
        }
        ImGui::SameLine();
        if (action_button("Show secrets...")) {
            widgets::request_show_secrets(secrets_state);
        }
    }
    ImGui::Spacing();

    draw_time_sync_banner();

    draw_account_picker(state);

    auto* a = state.find_account(state.selected_account_id);
    auto draw_modals = [&] {
        widgets::draw_backup_modal(state, backup_state);
        widgets::draw_export_mafiles_modal(state, export_state);
        widgets::draw_show_secrets_modal(state, secrets_state);
        widgets::draw_add_sda_modal(state, add_sda_state);
        widgets::draw_remove_sda_modal(state, remove_sda_state);
    };
    if (!a) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextWrapped("Pick an account with Steam Guard attached to see a code.");
        ImGui::PopStyleColor();
        draw_modals();
        return;
    }
    if (!a->sda.has_value()) {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "This account doesn't have Steam Guard attached. You can either import "
            "an existing maFile from the Add Account screen, or link a new "
            "authenticator now if the account already has a logged-in session "
            "stored in the vault.");
        ImGui::Spacing();
        const bool has_session = !a->access_token.empty();
        ImGui::BeginDisabled(!has_session);
        if (action_button("Add Steam Guard")) {
            widgets::request_add_sda(state, add_sda_state, a->id);
        }
        ImGui::EndDisabled();
        if (!has_session) {
            hover_tooltip("This account has no stored session. Run the full mobile "
                          "login wizard from the Add Account screen first, then "
                          "come back here.");
        }
        draw_modals();
        return;
    }

    ImGui::PushID(a->id.c_str());

    ImGui::Spacing();
    ImGui::SeparatorText("Account");
    ImGui::Spacing();
    draw_account_header(state, *a);

    if (!a->sda->fully_enrolled) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("Linking is incomplete. The revocation code is saved "
                           "but the activation code was never confirmed.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (action_button("Finish activation")) {
            widgets::request_add_sda(state, add_sda_state, a->id);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Current code");
    ImGui::Spacing();

    if (!time_aligner::synced()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextWrapped("Waiting on Steam time sync...");
        ImGui::PopStyleColor();
    } else {
        const std::int64_t now = time_aligner::aligned_now();
        const std::string code = sda::generate_code(a->sda->shared_secret, now);
        const int remaining = sda::seconds_remaining(now);

        static std::unordered_map<std::string, std::int64_t> g_code_reveal_until;
        const bool hide = state.settings.sda.hide_current_code;
        const auto reveal_until = g_code_reveal_until[a->id];
        const bool revealed = !hide || reveal_until > now;
        const std::string visible_code = revealed ? code : std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");

        const ImVec2 code_pos = ImGui::GetCursorScreenPos();
        widgets::draw_code_display(visible_code, remaining);
        if (hide) {
            const ImVec2 cell = ImGui::GetItemRectSize();
            if (ImGui::IsMouseHoveringRect(code_pos,
                ImVec2(code_pos.x + cell.x, code_pos.y + cell.y)) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_code_reveal_until[a->id] = revealed ? 0 : (now + 5);
            }
        }

        if (state.settings.sda.show_next_code) {
            const std::string next = sda::generate_code(a->sda->shared_secret, now + 30);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::Text("next: %s", revealed ? next.c_str() : "*****");
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        if (action_button("Copy code")) {
            platform::clipboard::set_text_with_auto_clear(
                code, std::chrono::seconds(state.settings.clipboard_clear_seconds));
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        char info[96];
        std::snprintf(info, sizeof(info),
                      "synced with Steam, offset %lld s",
                      static_cast<long long>(time_aligner::offset_seconds()));
        ImGui::TextUnformatted(info);
        ImGui::PopStyleColor();

        static std::string g_last_picked_id;
        if (state.settings.sda.auto_copy_on_select &&
            g_last_picked_id != a->id && !code.empty()) {
            platform::clipboard::set_text_with_auto_clear(
                code, std::chrono::seconds(state.settings.clipboard_clear_seconds));
        }
        g_last_picked_id = a->id;
    }

    ImGui::Spacing();
    draw_revocation_section(state, *a);

    ImGui::Spacing();
    if (action_button("Remove Steam Guard")) {
        widgets::request_remove_sda(remove_sda_state, a->id);
    }
    hover_tooltip("Asks Steam to revoke the mobile authenticator. Triggers a "
                  "15-day market and trade hold.");

    ImGui::PopID();

    draw_modals();
}

}  // namespace sam::ui::screens
