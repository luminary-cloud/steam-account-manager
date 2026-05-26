#include "ui/screens/accounts_screen.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

#include <imgui.h>

#include "core/account_store/filter.hpp"
#include "core/launch/steam_launcher.hpp"
#include "core/sda/totp.hpp"
#include "platform/clipboard.hpp"
#include "ui/screens/accounts_list_view.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/account_card.hpp"
#include "ui/widgets/bundle_dialogs.hpp"
#include "ui/widgets/search_bar.hpp"

namespace sam::ui::screens {

namespace {

constexpr float kCardMinWidth = 360.0F;
constexpr float kGap = 14.0F;
constexpr float kGridInset = 10.0F;

void handle_card_action(app::AppState& state,
                        core::Account& a,
                        widgets::CardAction act) {
    switch (act) {
        case widgets::CardAction::Launch: {
            state.flush_pending_save();
            auto result = sam::launch::launch_account(a);
            if (result.status != sam::launch::LaunchStatus::Ok) {
                state.launch_error = result.message;
                ImGui::OpenPopup("Launch failed");
            }
            break;
        }
        case widgets::CardAction::CopyCode:
            if (a.sda) {
                const std::string code = sda::generate_code_now(a.sda->shared_secret);
                platform::clipboard::set_text_with_auto_clear(
                    code, std::chrono::seconds(state.settings.clipboard_clear_seconds));
            }
            break;
        case widgets::CardAction::OpenSda:
            state.selected_account_id = a.id;
            state.current_screen = app::Screen::Authenticator;
            break;
        case widgets::CardAction::Edit:
            state.selected_account_id = a.id;
            state.current_screen = app::Screen::AddAccount;
            break;
        case widgets::CardAction::Refresh:
            state.refresh_single_account(a.id);
            break;
        case widgets::CardAction::Remove:
            state.selected_account_id = a.id;
            ImGui::OpenPopup("Confirm removal");
            break;
        case widgets::CardAction::ToggleSelect:
            state.toggle_selected(a.id);
            break;
        case widgets::CardAction::None:
        case widgets::CardAction::Reveal:
            break;
    }
}

void draw_grid_body(app::AppState& state,
                    const std::vector<std::size_t>& visible) {
    ImGui::Indent(kGridInset);

    const float left_inset = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x - left_inset;
    const int columns = std::max(1,
        static_cast<int>((avail + kGap) / (kCardMinWidth + kGap)));
    const float card_w = (avail - kGap * static_cast<float>(columns - 1)) /
                         static_cast<float>(columns);

    int col = 0;
    for (std::size_t idx : visible) {
        if (col > 0) ImGui::SameLine(0.0F, kGap);
        auto& a = state.vault.accounts[idx];
        const auto act = widgets::draw_account_card(state, a, card_w);
        if (act != widgets::CardAction::None) {
            handle_card_action(state, a, act);
        }
        col = (col + 1) % columns;
    }
    ImGui::Unindent(kGridInset);
}

}  // namespace

void draw_accounts(app::AppState& state) {
    static widgets::ExportBundleState export_state;

    ImGui::TextUnformatted("Accounts");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", state.vault.accounts.size());

    ImGui::Spacing();

    widgets::draw_search_bar(state.search_query, 320.0F);
    ImGui::SameLine();
    {
        const char* label = state.selection_mode ? "Exit selection" : "Select";
        if (action_button(label)) {
            if (state.selection_mode) state.exit_selection_mode();
            else                       state.enter_selection_mode();
        }
    }
    ImGui::SameLine();
    {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t cooldown =
            (state.last_batch_refresh_unix != 0 &&
             now - state.last_batch_refresh_unix < 120)
                ? (120 - (now - state.last_batch_refresh_unix)) : 0;
        const bool no_api_key = state.settings.web_api_key.empty();
        const bool disabled = no_api_key || cooldown > 0;
        ImGui::BeginDisabled(disabled);
        if (action_button("Refresh all")) {
            state.refresh_account_data();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (no_api_key) {
                ImGui::SetTooltip("Add a Steam Web API key in Settings to enable batch refresh.");
            } else if (cooldown > 0) {
                ImGui::SetTooltip("Wait %llds before refreshing again.",
                                  static_cast<long long>(cooldown));
            } else {
                ImGui::SetTooltip("Refresh every account's public Steam data "
                                  "and (if enabled) GCPD page.");
            }
        }

        const int total = state.refresh_all_total.load(std::memory_order_relaxed);
        const int done  = state.refresh_all_done.load(std::memory_order_relaxed);
        if (total > 0 && done < total) {
            ImGui::SameLine();
            ImGui::TextDisabled("Refreshing %d/%d", done, total);
        }
    }
    if (state.settings.accounts_view == app::AccountsViewMode::List) {
        ImGui::SameLine();
        if (action_button("New group")) {
            ImGui::OpenPopup("New group");
        }
        draw_new_group_modal(state);
    }

    ImGui::Spacing();
    {
        ImGui::TextDisabled("Sort");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        const char* sort_labels =
            "Persona name (A-Z)\0"
            "Persona name (Z-A)\0"
            "Login (A-Z)\0"
            "Last login\0"
            "Created\0"
            "Premier rating\0"
            "Steam level\0";
        if (ImGui::Combo("##sort", &state.settings.accounts_sort, sort_labels)) {
            state.save_settings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Checkbox("Only banned", &state.settings.quick_filters.only_banned)) {
            state.save_settings();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Only cooldown", &state.settings.quick_filters.only_cooldown)) {
            state.save_settings();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Only Prime", &state.settings.quick_filters.only_prime)) {
            state.save_settings();
        }
    }

    core::AccountFilter filter;
    filter.query = state.search_query;
    filter.only_with_any_ban = state.settings.quick_filters.only_banned;
    filter.only_with_cooldown = state.settings.quick_filters.only_cooldown;
    filter.only_with_prime = state.settings.quick_filters.only_prime;
    filter.now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    filter.sort = static_cast<core::SortKey>(std::clamp(state.settings.accounts_sort,
                                                         0, 6));
    const auto visible = core::apply_filter(state.vault.accounts, filter);

    if (state.selection_mode) {
        ImGui::Spacing();
        if (action_button("Select all visible")) {
            for (std::size_t idx : visible) {
                state.selected_account_ids.insert(state.vault.accounts[idx].id);
            }
        }
        ImGui::SameLine();
        if (action_button("Clear")) {
            state.selected_account_ids.clear();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu selected", state.selected_account_ids.size());
        ImGui::SameLine();

        const bool none_selected = state.selected_account_ids.empty();
        ImGui::BeginDisabled(none_selected);
        if (action_button("Export selected...")) {
            std::vector<std::string> ids(state.selected_account_ids.begin(),
                                          state.selected_account_ids.end());
            widgets::request_export_bundle(export_state, std::move(ids));
        }
        ImGui::SameLine();
        if (action_button("Delete selected")) {
            ImGui::OpenPopup("Confirm bulk removal");
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0F, kGridInset));

    if (state.settings.accounts_view == app::AccountsViewMode::List) {
        auto res = draw_list_body(state, visible);
        if (res.action != widgets::CardAction::None) {
            if (auto* acc = state.find_account(res.action_account_id)) {
                handle_card_action(state, *acc, res.action);
            }
        }
    } else {
        draw_grid_body(state, visible);
    }

    if (begin_styled_modal("Confirm removal")) {
        ImGui::TextWrapped("Remove this account from the vault? This cannot be undone.");
        ImGui::Spacing();
        if (action_button("Remove", ImVec2(100, 0))) {
            auto& accs = state.vault.accounts;
            accs.erase(std::remove_if(accs.begin(), accs.end(),
                [&](const core::Account& x) { return x.id == state.selected_account_id; }),
                accs.end());
            state.selected_account_id.clear();
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) {
            state.selected_account_id.clear();
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }

    if (begin_styled_modal("Confirm bulk removal", 460.0F)) {
        const std::size_t n = state.selected_account_ids.size();
        ImGui::TextWrapped("Remove %zu account%s from the vault? This cannot be undone.",
                           n, n == 1 ? "" : "s");

        std::vector<const core::Account*> picked;
        picked.reserve(n);
        for (const auto& a : state.vault.accounts) {
            if (state.selected_account_ids.count(a.id) != 0) {
                picked.push_back(&a);
                if (picked.size() >= 5) break;
            }
        }
        ImGui::Indent(20.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        for (const auto* a : picked) {
            ImGui::BulletText("%s", a->login.c_str());
        }
        if (n > picked.size()) {
            ImGui::BulletText("... and %zu more", n - picked.size());
        }
        ImGui::PopStyleColor();
        ImGui::Unindent(20.0F);

        ImGui::Spacing();
        if (action_button("Remove", ImVec2(100, 0))) {
            auto& accs = state.vault.accounts;
            accs.erase(std::remove_if(accs.begin(), accs.end(),
                [&](const core::Account& x) {
                    return state.selected_account_ids.count(x.id) != 0;
                }),
                accs.end());
            state.selected_account_ids.clear();
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }

    if (begin_styled_modal("Launch failed")) {
        ImGui::TextWrapped("%s", state.launch_error.c_str());
        ImGui::Spacing();
        if (action_button("OK", ImVec2(80, 0))) {
            state.launch_error.clear();
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }

    widgets::draw_export_bundle_modal(state, export_state);
}

}  // namespace sam::ui::screens
