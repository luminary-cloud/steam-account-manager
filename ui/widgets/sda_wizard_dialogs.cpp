#include "ui/widgets/sda_wizard_dialogs.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "app/job_pump.hpp"
#include "core/log.hpp"
#include "core/sda/revoke.hpp"
#include "core/sda/totp.hpp"
#include "core/steam_login/session.hpp"
#include "core/time_aligner.hpp"
#include "platform/clipboard.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::widgets {

namespace {

constexpr const char* kAddPopupName    = "Add Steam Guard";
constexpr const char* kRemovePopupName = "Remove Steam Guard";

// Per-account reveal-until-unix for the R-code; masked by default.
std::unordered_map<std::string, std::int64_t> g_rcode_reveal_until;

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ensure_token_fresh(core::Account& a) {
    if (!steam_login::needs_refresh(a, 300)) return true;
    if (a.refresh_token.empty()) return false;
    return steam_login::refresh_access_token(a);
}

void dispatch_add(app::AppState& app, AddSdaDialogState& s) {
    const std::string aid = s.account_id;
    app::job_pump::submit([aid, &app, &s] {
        bool token_ok = true;
        sda::AddAuthenticatorResult result;

        {
            auto* a = app.find_account(aid);
            if (!a) {
                app.post_ui_callback([&s] {
                    s.step = AddSdaDialogState::Step::Failed;
                    s.error = "Account no longer exists in the vault.";
                });
                return;
            }
            if (steam_login::needs_refresh(*a, 300)) {
                token_ok = !a->refresh_token.empty() &&
                           steam_login::refresh_access_token(*a);
            }
            if (token_ok) {
                result = sda::add_authenticator(*a);
            }
        }

        app.post_ui_callback([&app, &s, aid, token_ok,
                              result = std::move(result)] () mutable {
            if (s.account_id != aid) return;
            if (!token_ok) {
                s.step = AddSdaDialogState::Step::Failed;
                s.error = "The session has expired and no refresh token is "
                          "available. Re-login from the Add Account screen, "
                          "then try again.";
                return;
            }
            if (!result.ok || !result.sda.has_value()) {
                s.step = AddSdaDialogState::Step::Failed;
                if (result.status_code != 0) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "Steam returned status %d.", result.status_code);
                    s.error = buf;
                } else {
                    s.error = "Steam did not accept the AddAuthenticator request.";
                }
                if (!result.raw_response.empty()) {
                    SAM_LOG_WARN("AddAuthenticator failed: {}", result.raw_response);
                }
                return;
            }

            auto* a = app.find_account(aid);
            if (!a) {
                s.step = AddSdaDialogState::Step::Failed;
                s.error = "Account no longer exists in the vault.";
                return;
            }

            a->sda = std::move(*result.sda);
            if (a->sda->account_name.empty()) {
                a->sda->account_name = a->login;
            }
            a->sda->fully_enrolled = false;

            app.vault_dirty = true;
            app.save_vault_if_dirty();
            app.flush_pending_save();

            s.working_text.clear();
            s.step = AddSdaDialogState::Step::ShowRcode;
        });
    });
}

void dispatch_verify_active(app::AppState& app, AddSdaDialogState& s);

void dispatch_finalize(app::AppState& app, AddSdaDialogState& s) {
    const std::string aid = s.account_id;
    const std::string sms_code = s.activation_buf.data();
    app::job_pump::submit([aid, sms_code, &app, &s] {
        sda::FinalizeResult result;
        bool token_ok = true;
        {
            auto* a = app.find_account(aid);
            if (!a) {
                app.post_ui_callback([&s] {
                    s.step = AddSdaDialogState::Step::Failed;
                    s.error = "Account no longer exists in the vault.";
                });
                return;
            }
            if (steam_login::needs_refresh(*a, 300)) {
                token_ok = !a->refresh_token.empty() &&
                           steam_login::refresh_access_token(*a);
            }
            if (token_ok) {
                result = sda::finalize_add(*a, sms_code);
            }
        }

        app.post_ui_callback([&app, &s, aid, token_ok,
                              result = std::move(result)] {
            if (s.account_id != aid) return;

            if (!token_ok) {
                s.step = AddSdaDialogState::Step::Failed;
                s.error = "Session expired. Re-login from the Add Account "
                          "screen and try again.";
                return;
            }

            if (result.ok) {
                auto* a = app.find_account(aid);
                if (a && a->sda.has_value()) {
                    a->sda->fully_enrolled = true;
                    app.vault_dirty = true;
                    app.save_vault_if_dirty();
                }
                s.step = AddSdaDialogState::Step::Done;
                return;
            }

            if (result.needs_retry) {
                s.bad_code_hint = true;
                s.activation_buf.fill(0);
                s.step = AddSdaDialogState::Step::AwaitActivation;
                return;
            }

            // Finalize was ambiguous (e.g. status 2 "already finalized") or out
            // of tries. Steam often activates anyway, so confirm against the live
            // status before declaring failure.
            s.working_text = "Checking the authenticator with Steam...";
            s.step = AddSdaDialogState::Step::Working;
            dispatch_verify_active(app, s);
        });
    });
}

void dispatch_verify_active(app::AppState& app, AddSdaDialogState& s) {
    const std::string aid = s.account_id;
    app::job_pump::submit([aid, &app, &s] {
        sda::TwoFactorStatus status;
        bool token_ok = true;
        std::string mafile_token_gid;
        {
            auto* a = app.find_account(aid);
            if (!a || !a->sda.has_value()) {
                app.post_ui_callback([&s] {
                    s.step = AddSdaDialogState::Step::Failed;
                    s.error = "Account no longer exists in the vault.";
                });
                return;
            }
            mafile_token_gid = a->sda->token_gid;
            if (steam_login::needs_refresh(*a, 300)) {
                token_ok = !a->refresh_token.empty() &&
                           steam_login::refresh_access_token(*a);
            }
            if (token_ok) {
                status = sda::query_two_factor_status(*a);
            }
        }

        app.post_ui_callback([&app, &s, aid, token_ok, mafile_token_gid,
                              status = std::move(status)] {
            if (s.account_id != aid) return;

            if (!token_ok) {
                s.step = AddSdaDialogState::Step::Failed;
                s.error = "Session expired. Re-login from the Add Account "
                          "screen and try again.";
                return;
            }

            if (!status.ok) {
                s.step = AddSdaDialogState::Step::Failed;
                s.error = !status.error.empty()
                    ? "Could not verify with Steam: " + status.error
                    : "Could not reach Steam to verify the authenticator.";
                return;
            }

            // When the maFile has a token_gid it must match what Steam reports as
            // live; otherwise fall back to "a Valve mobile authenticator is active".
            const bool has_gid = !mafile_token_gid.empty();
            const bool steam_has_authenticator =
                status.authenticator_type == 1 || !status.token_gid.empty();
            const bool confirmed = has_gid
                ? (!status.token_gid.empty() && status.token_gid == mafile_token_gid)
                : status.authenticator_type == 1;

            if (confirmed) {
                if (auto* a = app.find_account(aid); a && a->sda.has_value()) {
                    a->sda->fully_enrolled = true;
                    app.vault_dirty = true;
                    app.save_vault_if_dirty();
                }
                s.step = AddSdaDialogState::Step::Done;
                return;
            }

            s.step = AddSdaDialogState::Step::Failed;
            s.error = (has_gid && steam_has_authenticator)
                ? "A different authenticator is active on this account, so the codes "
                  "in this maFile won't match it. Remove the other authenticator or "
                  "re-import the correct maFile."
                : "Steam reports no mobile authenticator is active on this account, "
                  "so this maFile can't be marked complete. It may have been removed "
                  "or was never finalized.";
        });
    });
}

void dispatch_remove(app::AppState& app, RemoveSdaDialogState& s) {
    const std::string aid = s.account_id;
    const int scheme = s.scheme;
    app::job_pump::submit([aid, scheme, &app, &s] {
        sda::RemoveResult result;
        bool token_ok = true;
        {
            auto* a = app.find_account(aid);
            if (!a) {
                app.post_ui_callback([&s] {
                    s.step = RemoveSdaDialogState::Step::Failed;
                    s.error = "Account no longer exists in the vault.";
                });
                return;
            }
            if (steam_login::needs_refresh(*a, 300)) {
                token_ok = !a->refresh_token.empty() &&
                           steam_login::refresh_access_token(*a);
            }
            if (token_ok) {
                result = sda::remove_authenticator(*a, scheme);
            }
        }

        app.post_ui_callback([&app, &s, aid, token_ok,
                              result = std::move(result)] {
            if (s.account_id != aid) return;

            if (!token_ok) {
                s.step = RemoveSdaDialogState::Step::Failed;
                s.error = "Session expired. Re-login from the Add Account "
                          "screen and try again.";
                return;
            }

            if (!result.ok) {
                s.step = RemoveSdaDialogState::Step::Failed;
                s.error = !result.error.empty()
                    ? result.error
                    : "RemoveAuthenticator failed.";
                return;
            }

            if (auto* a = app.find_account(aid)) {
                a->sda.reset();
                app.vault_dirty = true;
                app.save_vault_if_dirty();
                app.flush_pending_save();
            }
            s.step = RemoveSdaDialogState::Step::Done;
        });
    });
}

}  // namespace

void request_add_sda(app::AppState& app, AddSdaDialogState& s, std::string account_id) {
    s.account_id = std::move(account_id);
    s.activation_buf.fill(0);
    s.error.clear();
    s.working_text.clear();
    s.acknowledged_rcode = false;
    s.bad_code_hint = false;
    s.open = true;

    auto* a = app.find_account(s.account_id);
    if (a && a->sda.has_value() && !a->sda->fully_enrolled) {
        // Pick up a half-finished link rather than calling AddAuthenticator
        // again, which would burn another revocation code.
        s.acknowledged_rcode = true;
        s.step = AddSdaDialogState::Step::AwaitActivation;
    } else {
        s.working_text = "Preparing...";
        s.step = AddSdaDialogState::Step::Working;
        dispatch_add(app, s);
    }
    s.pending_open = true;
}

void request_verify_sda(app::AppState& app, AddSdaDialogState& s, std::string account_id) {
    s.account_id = std::move(account_id);
    s.activation_buf.fill(0);
    s.error.clear();
    s.working_text = "Checking the authenticator with Steam...";
    s.acknowledged_rcode = true;
    s.bad_code_hint = false;
    s.open = true;
    s.step = AddSdaDialogState::Step::Working;
    s.pending_open = true;
    dispatch_verify_active(app, s);
}

void request_remove_sda(RemoveSdaDialogState& s, std::string account_id) {
    s.account_id = std::move(account_id);
    s.code_check.fill(0);
    s.error.clear();
    s.scheme = 1;
    s.step = RemoveSdaDialogState::Step::Confirm;
    s.open = true;
    s.pending_open = true;
}

namespace {

void draw_rcode_block(core::Account& a) {
    const std::int64_t now = unix_now();
    const auto until = g_rcode_reveal_until[a.id];
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
        g_rcode_reveal_until[a.id] = revealed ? 0 : (now + 10);
    }
    ImGui::SameLine();
    if (action_button("Copy")) {
        platform::clipboard::set_text(a.sda->revocation_code);
    }
}

void draw_add_body(app::AppState& app, AddSdaDialogState& s) {
    auto* a = app.find_account(s.account_id);
    if (!a) {
        ImGui::TextWrapped("Account no longer exists in the vault.");
        ImGui::Spacing();
        if (action_button("Close")) ImGui::CloseCurrentPopup();
        return;
    }

    ImGui::TextDisabled("%s", a->login.c_str());
    ImGui::Spacing();

    switch (s.step) {
        case AddSdaDialogState::Step::Working: {
            ImGui::TextWrapped("%s", s.working_text.empty()
                ? "Working..." : s.working_text.c_str());
            break;
        }
        case AddSdaDialogState::Step::ShowRcode: {
            if (!a->sda.has_value()) {
                ImGui::TextWrapped("Steam Guard data is missing from the vault.");
                break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("Write this revocation code down before continuing. "
                               "It is the only way to remove the authenticator if "
                               "you lose access to it, and Steam cannot regenerate it.");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::SeparatorText("Revocation code");
            ImGui::Spacing();
            draw_rcode_block(*a);
            ImGui::Spacing();

            ImGui::Checkbox("I have written this code down somewhere safe",
                            &s.acknowledged_rcode);

            ImGui::Spacing();
            ImGui::BeginDisabled(!s.acknowledged_rcode);
            if (action_button("Next")) {
                s.bad_code_hint = false;
                s.activation_buf.fill(0);
                s.step = AddSdaDialogState::Step::AwaitActivation;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (action_button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            break;
        }
        case AddSdaDialogState::Step::AwaitActivation: {
            ImGui::TextWrapped("Steam sent an activation code to your email or phone. "
                               "Enter it below to finish linking.");
            ImGui::Spacing();

            if (s.bad_code_hint) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
                ImGui::TextUnformatted("That code was rejected. Try the latest one.");
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            ImGui::SetNextItemWidth(180.0F);
            const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CharsUppercase |
                                              ImGuiInputTextFlags_AutoSelectAll |
                                              ImGuiInputTextFlags_EnterReturnsTrue;
            const bool submitted = ImGui::InputText("Activation code",
                                                     s.activation_buf.data(),
                                                     s.activation_buf.size(),
                                                     flags);

            const bool has_code = s.activation_buf[0] != '\0';
            ImGui::Spacing();
            ImGui::BeginDisabled(!has_code);
            const bool clicked = action_button("Confirm");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (action_button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }

            if ((clicked || submitted) && has_code) {
                s.bad_code_hint = false;
                s.working_text = "Verifying...";
                s.step = AddSdaDialogState::Step::Working;
                dispatch_finalize(app, s);
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Already activated this authenticator elsewhere?");
            if (action_button("My authenticator already works")) {
                s.bad_code_hint = false;
                s.working_text = "Checking the authenticator with Steam...";
                s.step = AddSdaDialogState::Step::Working;
                dispatch_verify_active(app, s);
            }
            break;
        }
        case AddSdaDialogState::Step::Done: {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
            ImGui::TextWrapped("Steam Guard is linked. You can now generate codes "
                               "from the Authenticator screen and approve trade "
                               "confirmations from this app.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (action_button("Close")) ImGui::CloseCurrentPopup();
            break;
        }
        case AddSdaDialogState::Step::Failed: {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", s.error.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (a->sda.has_value() && !a->sda->fully_enrolled) {
                ImGui::TextWrapped("A partial link is stored in the vault. Reopen this "
                                   "wizard later to enter the activation code.");
                ImGui::Spacing();
            }
            if (action_button("Close")) ImGui::CloseCurrentPopup();
            break;
        }
        case AddSdaDialogState::Step::Idle:
            break;
    }
}

void draw_remove_body(app::AppState& app, RemoveSdaDialogState& s) {
    auto* a = app.find_account(s.account_id);
    if (!a) {
        ImGui::TextWrapped("Account no longer exists in the vault.");
        ImGui::Spacing();
        if (action_button("Close")) ImGui::CloseCurrentPopup();
        return;
    }
    if (!a->sda.has_value() && s.step != RemoveSdaDialogState::Step::Done) {
        ImGui::TextWrapped("This account has no Steam Guard attached.");
        ImGui::Spacing();
        if (action_button("Close")) ImGui::CloseCurrentPopup();
        return;
    }

    ImGui::TextDisabled("%s", a->login.c_str());
    ImGui::Spacing();

    switch (s.step) {
        case RemoveSdaDialogState::Step::Confirm: {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("Removing Steam Guard triggers a 15-day market and trade "
                               "hold. The revocation code below cannot be regenerated, "
                               "so keep a copy if you might need to undo this.");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::SeparatorText("Revocation code");
            ImGui::Spacing();
            if (a->sda->revocation_code.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
                ImGui::TextWrapped("No revocation code is stored for this account. "
                                   "Steam will refuse the removal without it.");
                ImGui::PopStyleColor();
            } else {
                draw_rcode_block(*a);
            }
            ImGui::Spacing();

            ImGui::SeparatorText("What happens after");
            ImGui::Spacing();
            ImGui::RadioButton("Switch to email Steam Guard", &s.scheme, 1);
            hover_tooltip("Steam Guard stays on. Login codes are sent to the "
                          "account's email instead of the mobile authenticator.");
            ImGui::RadioButton("Remove Steam Guard entirely", &s.scheme, 2);
            hover_tooltip("No Steam Guard at all. Only the password protects the "
                          "account. Not recommended.");
            ImGui::Spacing();

            ImGui::SeparatorText("Confirm with current code");
            ImGui::Spacing();
            const std::int64_t now_aligned = time_aligner::aligned_now();
            const std::string expected = time_aligner::synced()
                ? sda::generate_code(a->sda->shared_secret, now_aligned)
                : std::string{};

            ImGui::TextWrapped("Type the current Steam Guard code shown on the "
                               "Authenticator screen. This is a local sanity check.");
            ImGui::SetNextItemWidth(140.0F);
            ImGui::InputText("##remove-code", s.code_check.data(), s.code_check.size(),
                              ImGuiInputTextFlags_CharsUppercase |
                              ImGuiInputTextFlags_AutoSelectAll);

            const std::string typed = s.code_check.data();
            const bool code_ok = !expected.empty() && typed.size() == expected.size() &&
                                 std::equal(typed.begin(), typed.end(), expected.begin(),
                                            [](char l, char r) {
                                                return std::toupper(static_cast<unsigned char>(l)) ==
                                                       std::toupper(static_cast<unsigned char>(r));
                                            });

            if (!expected.empty() && !typed.empty() && !code_ok) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
                ImGui::TextUnformatted("Code does not match the current authenticator.");
                ImGui::PopStyleColor();
            }
            if (expected.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
                ImGui::TextUnformatted("Waiting for Steam time sync...");
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            const bool can_remove = code_ok && !a->sda->revocation_code.empty();
            ImGui::BeginDisabled(!can_remove);
            if (action_button("Remove Steam Guard")) {
                s.step = RemoveSdaDialogState::Step::Working;
                dispatch_remove(app, s);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (action_button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            break;
        }
        case RemoveSdaDialogState::Step::Working: {
            ImGui::TextWrapped("Asking Steam to remove the authenticator...");
            break;
        }
        case RemoveSdaDialogState::Step::Done: {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
            ImGui::TextWrapped(s.scheme == 2
                ? "Steam Guard has been removed from this account."
                : "Steam Guard switched to email codes for this account.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (action_button("Close")) ImGui::CloseCurrentPopup();
            break;
        }
        case RemoveSdaDialogState::Step::Failed: {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", s.error.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (action_button("Back")) {
                s.error.clear();
                s.step = RemoveSdaDialogState::Step::Confirm;
            }
            ImGui::SameLine();
            if (action_button("Close")) ImGui::CloseCurrentPopup();
            break;
        }
        case RemoveSdaDialogState::Step::Idle:
            break;
    }
}

}  // namespace

void draw_add_sda_modal(app::AppState& app, AddSdaDialogState& s) {
    if (s.pending_open) {
        ImGui::OpenPopup(kAddPopupName);
        s.pending_open = false;
    }
    if (!s.open) return;

    ImGui::SetNextWindowSize(ImVec2(500.0F, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool visible = ImGui::BeginPopupModal(
        kAddPopupName, &s.open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!visible) {
        ImGui::PopStyleVar(2);
        if (s.step == AddSdaDialogState::Step::Done ||
            s.step == AddSdaDialogState::Step::Failed) {
            s.step = AddSdaDialogState::Step::Idle;
            s.account_id.clear();
            s.activation_buf.fill(0);
        }
        return;
    }

    draw_add_body(app, s);

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

void draw_remove_sda_modal(app::AppState& app, RemoveSdaDialogState& s) {
    if (s.pending_open) {
        ImGui::OpenPopup(kRemovePopupName);
        s.pending_open = false;
    }
    if (!s.open) return;

    ImGui::SetNextWindowSize(ImVec2(500.0F, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool visible = ImGui::BeginPopupModal(
        kRemovePopupName, &s.open,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!visible) {
        ImGui::PopStyleVar(2);
        if (s.step == RemoveSdaDialogState::Step::Done ||
            s.step == RemoveSdaDialogState::Step::Failed) {
            s.step = RemoveSdaDialogState::Step::Idle;
            s.account_id.clear();
            s.code_check.fill(0);
        }
        return;
    }

    draw_remove_body(app, s);

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

}  // namespace sam::ui::widgets
