#include "ui/screens/sda_screen.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "core/sda/totp.hpp"
#include "core/time_aligner.hpp"
#include "platform/clipboard.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/code_display.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/sda_backup_dialogs.hpp"
#include "ui/widgets/sda_wizard_dialogs.hpp"

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

bool draw_account_chip(app::AppState& state, const core::Account* picked) {
    constexpr float kH = 40.0F;
    constexpr float kW = 320.0F;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##sda_acct_chip", ImVec2(kW, kH));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p1(p0.x + kW, p0.y + kH);
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1,
                      ImGui::ColorConvertFloat4ToU32(hov ? theme::panel_hover() : theme::panel()),
                      6.0F);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::border()), 6.0F, 1.0F);
    const float pad = 6.0F;
    const float av = kH - 2.0F * pad;
    const ImVec2 av0(p0.x + pad, p0.y + pad);
    const ImVec2 av1(av0.x + av, av0.y + av);
    const float av_round = av * (8.0F / 48.0F);
    dl->AddRectFilled(av0, av1, IM_COL32(40, 50, 60, 180), av_round);
    if (picked != nullptr)
        if (auto* srv = widgets::avatar_for(picked->web.avatar_url_full))
            dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv), av0, av1, ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, av_round);
    const std::string label =
        picked != nullptr ? account_label(state, *picked) : std::string("Select an account");
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    const float tx0 = av1.x + 8.0F;
    dl->PushClipRect(ImVec2(tx0, p0.y), ImVec2(p1.x - pad, p1.y), true);
    dl->AddText(ImVec2(tx0, p0.y + (kH - ts.y) * 0.5F),
                ImGui::ColorConvertFloat4ToU32(picked != nullptr ? theme::text() : theme::dim_text()),
                label.c_str());
    dl->PopClipRect();
    return clicked;
}

void draw_account_grid(app::AppState& state, char (&search)[64]) {
    ImGui::SetNextItemWidth(220.0F);
    ImGui::InputTextWithHint("##sdaacctsearch", "filter", search, sizeof(search));

    const std::string af = lowercase(search);
    std::vector<core::Account*> accts;
    for (auto& a : state.vault.accounts) {
        if (!af.empty() && lowercase(account_label(state, a)).find(af) == std::string::npos)
            continue;
        accts.push_back(&a);
    }

    constexpr float kChipH = 44.0F;
    constexpr float kChipGap = 8.0F;
    ImGui::BeginChild("##sdaacctgrid", ImVec2(440.0F, 300.0F), ImGuiChildFlags_Borders);
    const float gw = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>(gw / 220.0F + 0.5F));
    const float chip_w = (gw - static_cast<float>(cols - 1) * kChipGap) / static_cast<float>(cols);
    const float row_pitch = kChipH + ImGui::GetStyle().ItemSpacing.y;
    const int total = static_cast<int>(accts.size());
    const int rows = (total + cols - 1) / cols;
    auto* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(rows, row_pitch);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int c = 0; c < cols; ++c) {
                const int idx = row * cols + c;
                if (idx >= total) break;
                core::Account& a = *accts[idx];
                if (c > 0) ImGui::SameLine(0.0F, kChipGap);
                ImGui::PushID(a.id.c_str());
                const bool sel = state.selected_account_id == a.id;
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p1(p0.x + chip_w, p0.y + kChipH);
                if (ImGui::InvisibleButton("##chip", ImVec2(chip_w, kChipH))) {
                    state.selected_account_id = a.id;
                    ImGui::CloseCurrentPopup();
                }
                const bool hov = ImGui::IsItemHovered();
                ImU32 bg;
                if (sel) {
                    ImVec4 ac = theme::accent();
                    ac.w = 0.20F;
                    bg = ImGui::ColorConvertFloat4ToU32(ac);
                } else {
                    bg = ImGui::ColorConvertFloat4ToU32(hov ? theme::panel_hover() : theme::panel());
                }
                dl->AddRectFilled(p0, p1, bg, 6.0F);

                const float pad = 6.0F;
                const float av = kChipH - 2.0F * pad;
                const ImVec2 av0(p0.x + pad, p0.y + pad);
                const ImVec2 av1(av0.x + av, av0.y + av);
                const float av_round = av * (8.0F / 48.0F);
                dl->AddRectFilled(av0, av1, IM_COL32(40, 50, 60, 180), av_round);
                if (auto* srv = widgets::avatar_for(a.web.avatar_url_full))
                    dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv), av0, av1, ImVec2(0, 0),
                                        ImVec2(1, 1), IM_COL32_WHITE, av_round);

                std::string label = account_label(state, a);
                if (!a.sda.has_value()) label += "  (no SDA)";
                else if (!a.sda->fully_enrolled) label += "  (incomplete)";

                const float tx0 = av1.x + 8.0F;
                const float tx1 = p1.x - pad;
                const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                const float region_w = tx1 - tx0;
                const float text_y = p0.y + (kChipH - ts.y) * 0.5F;
                dl->PushClipRect(ImVec2(tx0, p0.y), ImVec2(tx1, p1.y), true);
                dl->AddText(ImVec2(tx0, text_y), ImGui::ColorConvertFloat4ToU32(theme::text()),
                            label.c_str());
                dl->PopClipRect();
                if (sel)
                    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::accent()), 6.0F, 2.0F);
                if (hov && ts.x > region_w) set_tooltip("%s", label.c_str());
                ImGui::PopID();
            }
        }
    }
    clipper.End();
    ImGui::EndChild();
}

void draw_account_picker(app::AppState& state) {
    static char picker_search[64] = "";

    auto* current = state.find_account(state.selected_account_id);
    if (draw_account_chip(state, current)) {
        picker_search[0] = '\0';
        ImGui::OpenPopup("##sda_account_picker");
    }
    if (begin_styled_popup("##sda_account_picker")) {
        draw_account_grid(state, picker_search);
        end_styled_popup();
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

    separator_text("Revocation code");
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
    } else {

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
        hover_tooltip("Stores it in the vault. Steam cannot regenerate this code.");
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
            hover_tooltip("No stored session. Run the full mobile login from Add "
                          "Account first.");
        }
        draw_modals();
        return;
    }

    ImGui::PushID(a->id.c_str());

    ImGui::Spacing();
    separator_text("Account");
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
        ImGui::SameLine();
        if (action_button("Already activated")) {
            widgets::request_verify_sda(state, add_sda_state, a->id);
        }
        hover_tooltip("Use this if Steam Guard already works here. Confirms with Steam "
                      "and clears the warning, without an activation code.");
    }

    ImGui::Spacing();
    separator_text("Current code");
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
    hover_tooltip("Revokes the mobile authenticator. Triggers a 15-day trade hold.");

    ImGui::PopID();

    draw_modals();
}

}  // namespace sam::ui::screens
