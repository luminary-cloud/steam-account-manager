#include "ui/screens/accounts_screen.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/app_paths.hpp"
#include "app/gamesense_loader.hpp"
#include "app/luminary_loader.hpp"
#include "app/job_pump.hpp"
#include "core/account_store/filter.hpp"
#include "core/hwid/hwid_gen.hpp"
#include "core/launch/cs2_autostart.hpp"
#include "core/launch/steam_launcher.hpp"
#include "core/steam_login/session.hpp"
#include "core/strings.hpp"
#include "core/profile/edit.hpp"
#include "core/sda/totp.hpp"
#include "platform/clipboard.hpp"
#include "platform/file_dialog.hpp"
#include "ui/screens/accounts_list_view.hpp"
#include "ui/screens/add_account_screen.hpp"
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

// Seconds of headroom on the token's expiry. A launch shuts Steam down and restarts it, so a
// token that only just validates here is dead by the time Steam presents it. Matches the
// autopull's kTokenExpiryMargin.
constexpr std::int64_t kCmTokenExpiryMargin = 300;

bool cm_token_valid(const core::Account& a) {
    if (a.cm_refresh_token.empty()) return false;
    // A revoked token keeps a valid `exp` for months, so only a real sign-in attempt can
    // tell us it's dead; verify_cm_token_signin_async records that verdict here.
    if (a.cm_status == core::NfaTokenStatus::Revoked) return false;
    const std::int64_t exp = a.cm_refresh_token_expires != 0
        ? a.cm_refresh_token_expires
        : steam_login::jwt_expiry(a.cm_refresh_token);
    if (exp != 0 && exp - kCmTokenExpiryMargin <=
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())
        return false;
    return steam_login::jwt_audience(a.cm_refresh_token).find("client") != std::string::npos;
}

void do_launch(app::AppState& state, core::Account& a, bool use_token) {
    std::filesystem::path loader;
    if (a.login_method == core::LoginMethod::LaunchCs2Gamesense) {
        if (auto p = app::gamesense_loader_path()) loader = *p;
    }
    std::filesystem::path lum_loader;
    if (a.login_method == core::LoginMethod::LaunchCs2Luminary) {
        if (auto p = app::luminary_loader_path()) lum_loader = *p;
    }
    // Before re-injecting for an NFA account, adopt any rotated token Steam left from a
    // prior sign-in, so we never launch with a token that's already been superseded.
    if (a.is_nfa) {
        state.capture_rotated_token_now(a.id, core::to_lower(a.login));
    }
    auto result = sam::launch::launch_account(
        a, state.settings.cs2_video.launch_options,
        state.settings.disable_cloud_on_login, state.settings.disable_news_on_login,
        state.settings.disable_workshop_on_login,
        state.settings.remember_password_on_login, loader, lum_loader,
        state.settings.hwid.component_mask, use_token);
    if (result.status != sam::launch::LaunchStatus::Ok) {
        state.launch_error = result.message;
        ImGui::OpenPopup("Launch failed");
        return;
    }
    if (result.hwid_outcome == sam::launch::InjectOutcome::Success) {
        state.last_hwid_account_id = a.id;
        ui::widgets::ToastItem t;
        t.id = "hwid-" + a.id;
        t.message = "HWID spoofed for " + a.login;
        t.account_id = a.id;
        t.is_warning = true;
        t.expires_at_unix = now_seconds() + state.settings.notifications.toast_duration_seconds;
        state.toasts.push(std::move(t));
    } else if (result.hwid_outcome == sam::launch::InjectOutcome::Failed) {
        ui::widgets::ToastItem t;
        t.id = "hwid-fail-" + a.id;
        t.message = "HWID spoof failed for " + a.login + ": " + result.hwid_error;
        t.account_id = a.id;
        t.expires_at_unix = now_seconds() + state.settings.notifications.toast_duration_seconds;
        state.toasts.push(std::move(t));
    }
    a.last_login_unix = now_seconds();
    state.vault_dirty = true;
    state.save_vault_if_dirty();
    // After sign-in Steam rotates the refresh token in its ConnectCache, so read it back to
    // keep the stored token current (a stale one drops to the login form). The two token
    // kinds live in different fields, so each has its own watcher: an NFA account's login
    // token is refresh_token, while a use_token account signs in with cm_refresh_token.
    // Both also treat "Steam never signed in" as the token being dead.
    if (a.is_nfa) {
        state.capture_rotated_token_async(a.id, a.steam_id_64, core::to_lower(a.login));
    } else if (use_token) {
        state.verify_cm_token_signin_async(a.id, a.steam_id_64, core::to_lower(a.login));
    }
    if (state.settings.cs2_video.mode != app::CS2ConfigMode::None) {
        state.apply_cs2_video_config(a);
    }
    if (a.login_method != core::LoginMethod::Normal && !result.first_login_deferred) {
        sam::launch::cs2_autostart::start_async(a.login_method, a.steam_id_64,
                                                std::move(loader), std::move(lum_loader));
    }
}

void handle_card_action(app::AppState& state,
                        core::Account& a,
                        widgets::CardAction act) {
    switch (act) {
        case widgets::CardAction::Launch: {
            state.flush_pending_save();
            if (state.settings.hwid.always_spoof && !a.hwid_excluded && !a.hwid.has_value()) {
                a.hwid = core::hwid::generate_profile();
                state.vault_dirty = true;
                state.save_vault_if_dirty();
            }
            const bool use_token =
                !a.is_nfa &&
                state.settings.sign_in_method == app::SignInMethod::TokenInject;

            // Fresh user intent: allow the self-heal retry again, even if a previous launch
            // of this account already burned its one attempt.
            state.cm_relaunch_attempted.erase(a.id);

            if (use_token && !cm_token_valid(a)) {
                if (a.password.empty()) {
                    state.launch_error =
                        "Token injection needs a stored password to mint the login "
                        "token, but this account has no password.";
                    ImGui::OpenPopup("Launch failed");
                    break;
                }
                state.pending_token_launch = {a.id, true, false, false, {}};
                const std::string aid = a.id;
                state.acquire_cm_token(aid, [&state, aid](bool ok, std::string err) {
                    state.pending_token_launch.minting = false;
                    state.pending_token_launch.mint_done = true;
                    state.pending_token_launch.mint_ok = ok;
                    state.pending_token_launch.mint_error = std::move(err);
                });
                {
                    ui::widgets::ToastItem t;
                    t.id = "token-mint-" + a.id;
                    t.message = "Minting login token for " + a.login + "...";
                    t.account_id = a.id;
                    t.expires_at_unix = now_seconds() + 15;
                    state.toasts.push(std::move(t));
                }
                break;
            }

            do_launch(state, a, use_token);
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
            state.account_edit_requested = true;
            break;
        case widgets::CardAction::Refresh:
            // Per-account "refresh everything", not cache-gated: Steam Web API + (full-access)
            // GCPD, external funds, and an own-session GC pull (cooldown + ranks/medals). The GC
            // pull also serves as the NFA/cached token validation.
            state.refresh_single_account(a.id);
            if (!a.is_nfa) state.refresh_spend(a.id, /*quiet=*/true);
            state.queue_gc_validate(a.id);
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

    // Fixed card height lets the clipper virtualize: only on-screen rows get built.
    const int rows = (static_cast<int>(visible.size()) + columns - 1) / columns;
    const float row_pitch =
        widgets::kAccountCardHeight + ImGui::GetStyle().ItemSpacing.y;

    ImGuiListClipper clipper;
    clipper.Begin(rows, row_pitch);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            ImGui::SetCursorPosX(left_inset);
            for (int c = 0; c < columns; ++c) {
                const std::size_t vi =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                    static_cast<std::size_t>(c);
                if (vi >= visible.size()) break;
                if (c > 0) ImGui::SameLine(0.0F, kGap);
                auto& a = state.vault.accounts[visible[vi]];
                const auto act = widgets::draw_account_card(state, a, card_w);
                if (act != widgets::CardAction::None) {
                    handle_card_action(state, a, act);
                }
            }
        }
    }
    clipper.End();

    ImGui::Unindent(kGridInset);
}

// Owned here, not in the per-card context menu: that menu is nested under
// PushID(account), so a modal there would get an unstable ID scope. The context menu
// signals via state.persona_change_requested + selected_account_id.
void draw_change_username_modal(app::AppState& state) {
    static std::array<char, 128> name_buf{};
    static bool busy = false;
    static std::string status;
    static bool status_error = false;

    if (state.persona_change_requested) {
        state.persona_change_requested = false;
        busy = false;
        status.clear();
        name_buf.fill('\0');
        if (const auto* acc = state.find_account(state.selected_account_id)) {
            std::snprintf(name_buf.data(), name_buf.size(), "%s",
                          acc->web.persona_name.c_str());
        }
        ImGui::OpenPopup("Change username");
    }

    if (!begin_styled_modal("Change username")) return;

    const core::Account* acc = state.find_account(state.selected_account_id);
    if (acc == nullptr) {
        ImGui::TextDisabled("Account no longer exists.");
        if (action_button("Close", ImVec2(80, 0))) ImGui::CloseCurrentPopup();
        end_styled_modal();
        return;
    }

    ImGui::TextUnformatted("New Steam display name");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::BeginDisabled(busy);
    ImGui::InputText("##new-persona", name_buf.data(), name_buf.size());
    ImGui::EndDisabled();

    if (!status.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              status_error ? theme::danger() : theme::success());
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
    } else if (busy) {
        ImGui::Spacing();
        ImGui::TextDisabled("Applying...");
    }

    const std::int64_t cooldown = state.persona_change_cooldown_seconds(acc->id);
    if (cooldown > 0) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        if (cooldown >= 60) {
            ImGui::Text("Rate limited - try again in %lldm %02llds",
                        static_cast<long long>(cooldown / 60),
                        static_cast<long long>(cooldown % 60));
        } else {
            ImGui::Text("Rate limited - try again in %llds",
                        static_cast<long long>(cooldown));
        }
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const bool name_ok = name_buf[0] != '\0';
    ImGui::BeginDisabled(busy || !name_ok || cooldown > 0);
    if (action_button("Apply", ImVec2(100, 0))) {
        busy = true;
        status.clear();
        const std::string new_name = name_buf.data();
        core::Account snapshot = *acc;
        app::job_pump::submit(
            [&state, snapshot, new_name]() mutable {
                auto res = sam::profile::change_persona_name(snapshot, new_name);
                if (!res.ok && res.needs_relogin) {
                    std::string err;
                    if (state.auto_relogin(snapshot.id, snapshot, &err)) {
                        res = sam::profile::change_persona_name(snapshot, new_name);
                    }
                }
                state.post_ui_callback(
                    [&state, snapshot = std::move(snapshot),
                     new_name = std::move(new_name), res]() mutable {
                        const std::string shown =
                            res.applied_name.empty() ? new_name : res.applied_name;
                        if (auto* a = state.find_account(snapshot.id)) {
                            a->refresh_token        = snapshot.refresh_token;
                            a->access_token         = snapshot.access_token;
                            a->access_token_expires = snapshot.access_token_expires;
                            a->steam_login_secure   = snapshot.steam_login_secure;
                            a->session_id           = snapshot.session_id;
                            if (res.ok) {
                                a->web.persona_name = shown;
                                state.last_persona_change_unix[snapshot.id] =
                                    sam::ui::now_seconds();
                            }
                            state.vault_dirty = true;
                            state.save_vault_if_dirty();
                        }
                        // Skip if the modal switched accounts, so a stale result
                        // doesn't show on a different account.
                        if (state.selected_account_id != snapshot.id) return;
                        busy = false;
                        if (res.ok) {
                            status = "Display name changed to \"" + shown + "\".";
                            status_error = false;
                        } else {
                            status = res.error.empty()
                                ? std::string("Failed to change the name.")
                                : res.error;
                            status_error = true;
                        }
                    });
            });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (action_button(status.empty() ? "Cancel" : "Close", ImVec2(100, 0))) {
        name_buf.fill('\0');
        status.clear();
        busy = false;
        ImGui::CloseCurrentPopup();
    }
    end_styled_modal();
}

}  // namespace

void draw_accounts(app::AppState& state) {
    static widgets::ExportBundleState export_state;

    ImGui::TextUnformatted("Accounts");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", state.vault.accounts.size());

    if (!state.warned_missing_api_key &&
        !state.vault.accounts.empty() &&
        state.settings.web_api_key.empty()) {
        widgets::ToastItem t;
        t.id = "missing-api-key";
        t.message = "No Steam Web API key set, so account data won't be refreshed. "
                    "Click here to add one in Settings.";
        t.is_warning = true;
        t.on_click_action = widgets::ToastClickAction::Settings;
        t.expires_at_unix = 0;  // persist until the user dismisses it
        state.toasts.push(std::move(t));
        state.warned_missing_api_key = true;
    }

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
        // No time cooldown: the refresh is cache-gated, so a repeat click just re-checks and
        // no-ops ("up to date"). Only a missing API key disables it.
        const bool no_api_key = state.settings.web_api_key.empty();
        ImGui::BeginDisabled(no_api_key);
        if (action_button("Refresh all")) {
            state.refresh_account_data(/*force=*/false, /*announce=*/true);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (no_api_key) {
                set_tooltip("Add a Steam Web API key in Settings to enable batch refresh.");
            } else {
                set_tooltip("Refresh Steam data + GCPD (full-access) and the CS2 competitive "
                                  "cooldown (NFA/cached, over the GC). Skips accounts still "
                                  "within the cache window.");
            }
        }

        const int total = state.refresh_all_total.load(std::memory_order_relaxed);
        const int done  = state.refresh_all_done.load(std::memory_order_relaxed);
        if (total > 0 && done < total) {
            ImGui::SameLine();
            ImGui::TextDisabled("Refreshing %d/%d", done, total);
        }
    }
    ImGui::SameLine();
    {
        // "Refresh GC": batch-pull full-access profiles + per-account NFA own-session logins.
        // The batch (gc_autopull) is stoppable; the validate sweep runs to completion.
        const bool validate_busy =
            state.gc_validate.active && !state.gc_validate.feed_refresh_all;
        if (state.gc_autopull.active) {
            if (action_button("Stop GC pull")) state.cancel_gc_autopull();
        } else {
            ImGui::BeginDisabled(!state.settings.cs2_gc.enabled);
            if (action_button("Refresh GC")) state.refresh_gc_all(/*announce=*/true);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!state.settings.cs2_gc.enabled)
                    set_tooltip("Enable the CS2 Game Coordinator in Settings to use this.");
                else
                    set_tooltip("Pull CS2 GC data (level, medals, ranks, cooldown) for every "
                                "account: one puller queries full-access accounts by Steam ID, "
                                "NFA/cached each sign in. Skips accounts within the cache window.");
            }
        }
        // Combined progress across the batch pull and any standalone NFA validate sweep. A
        // feed sweep's progress shows under "Refresh all" instead, so it's excluded here.
        const int gc_done = state.gc_autopull.received +
                            (validate_busy ? state.gc_validate.done : 0);
        const int gc_total = state.gc_autopull.total +
                            (validate_busy ? static_cast<int>(state.gc_validate.queue.size()) : 0);
        if (gc_total > 0 && gc_done < gc_total) {
            ImGui::SameLine();
            ImGui::TextDisabled("GC %d/%d", gc_done, gc_total);
        }
    }
    if (state.settings.info.show_external_funds) {
        ImGui::SameLine();
        const bool busy = state.spend_bulk_running.load(std::memory_order_acquire);
        ImGui::BeginDisabled(busy);
        if (action_button("Refresh spent")) {
            state.refresh_all_spend(/*only_missing=*/false);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            set_tooltip("Sign in to each full-access account and read total external funds "
                        "(TotalSpend) from Steam Support. One sign-in per account. Skips "
                        "accounts within the cache window.");
        }
        const int sp_total = state.spend_total.load(std::memory_order_relaxed);
        const int sp_done  = state.spend_done.load(std::memory_order_relaxed);
        if (busy || (sp_total > 0 && sp_done < sp_total)) {
            ImGui::SameLine();
            if (sp_total > 0)
                ImGui::TextDisabled("Fetching funds %d/%d", sp_done, sp_total);
            else
                ImGui::TextDisabled("Fetching funds...");
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
            "Persona A-Z\0"
            "Persona Z-A\0"
            "Login A-Z\0"
            "Last login\0"
            "Created\0"
            "Premier\0"
            "Steam lvl\0"
            "CS2 lvl\0"
            "No cooldown\0"
            "Drop unclaimed\0"
            "CS2 XP\0"
            "Spend ($)\0"
            "Cooldown soon\0";
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        if (ImGui::Combo("##sort", &state.settings.accounts_sort, sort_labels)) {
            state.save_settings();
        }
        ImGui::PopStyleVar();
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
                                                         0, 12));
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
    separator();
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

    // Run here, not inside the per-card ImGui popup: a Win32 modal can't open from there.
    if (state.gamesense_pick_request.has_value()) {
        const std::string acc_id = *state.gamesense_pick_request;
        state.gamesense_pick_request.reset();

        platform::file_dialog::Options opts;
        opts.parent = state.main_hwnd;
        opts.title = L"Choose gamesense loader";
        opts.filters = {
            {L"Executable (*.exe)", L"*.exe"},
            {L"All files (*.*)", L"*.*"},
        };
        const auto picked = platform::file_dialog::open_file(opts);
        if (picked.ok) {
            std::string err;
            if (app::install_gamesense_loader(picked.path, &err)) {
                if (!acc_id.empty()) {
                    if (auto* acc = state.find_account(acc_id)) {
                        acc->login_method = core::LoginMethod::LaunchCs2Gamesense;
                        state.vault_dirty = true;
                        state.save_vault_if_dirty();
                    }
                }
            } else {
                state.launch_error = err.empty()
                    ? std::string("Could not install the gamesense loader.")
                    : err;
                ImGui::OpenPopup("Launch failed");
            }
        }
    }

    if (state.luminary_pick_request.has_value()) {
        const std::string acc_id = *state.luminary_pick_request;
        state.luminary_pick_request.reset();

        platform::file_dialog::Options opts;
        opts.parent = state.main_hwnd;
        opts.title = L"Choose luminary loader";
        opts.filters = {
            {L"Executable (*.exe)", L"*.exe"},
            {L"All files (*.*)", L"*.*"},
        };
        const auto picked = platform::file_dialog::open_file(opts);
        if (picked.ok) {
            std::string err;
            if (app::install_luminary_loader(picked.path, &err)) {
                if (!acc_id.empty()) {
                    if (auto* acc = state.find_account(acc_id)) {
                        acc->login_method = core::LoginMethod::LaunchCs2Luminary;
                        state.vault_dirty = true;
                        state.save_vault_if_dirty();
                    }
                }
            } else {
                state.launch_error = err.empty()
                    ? std::string("Could not install the luminary loader.")
                    : err;
                ImGui::OpenPopup("Launch failed");
            }
        }
    }

    if (state.pending_token_launch.mint_done) {
        auto& ptl = state.pending_token_launch;
        const std::string aid = ptl.account_id;
        const bool ok = ptl.mint_ok;
        std::string err = std::move(ptl.mint_error);
        ptl = {};
        if (!ok) {
            state.launch_error = "Could not mint login token: " + err;
            ImGui::OpenPopup("Launch failed");
        } else if (auto* acc = state.find_account(aid)) {
            do_launch(state, *acc, true);
        }
    }

    if (begin_styled_modal("Confirm removal")) {
        ImGui::TextWrapped("Remove this account from the vault? This cannot be undone.");
        ImGui::Spacing();
        if (action_button("Remove", ImVec2(100, 0))) {
            state.remove_accounts({state.selected_account_id});
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
            state.remove_accounts(state.selected_account_ids);
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

    draw_change_username_modal(state);
    draw_edit_notes_modal(state);
    draw_edit_account_modal(state);

    widgets::draw_export_bundle_modal(state, export_state);
}

}  // namespace sam::ui::screens
