#include "ui/screens/add_account_detail.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <imgui.h>

#include "app/job_pump.hpp"
#include "core/account_store/store.hpp"
#include "core/crypto/rng.hpp"
#include "core/log.hpp"
#include "core/sda/mafile.hpp"
#include "core/sda/info_dat.hpp"
#include "core/sda/totp.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/steam_login/mobile_auth.hpp"
#include "core/steam_login/session.hpp"
#include "core/time_aligner.hpp"
#include "platform/file_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/bundle_dialogs.hpp"
#include "ui/widgets/master_pw_field.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/stepper.hpp"

namespace sam::ui::screens {

namespace {

struct LoginWizard {
    enum class Phase { Credentials, GuardWait, Polling, Done, Error };
    Phase phase = Phase::Credentials;

    std::array<char, 64> username{};
    std::string password;
    std::string shared_secret;        // base64; if set, auto-generates Guard codes
    std::array<char, 8> guard_code{};

    std::string client_id;
    std::string request_id;
    std::uint64_t steam_id = 0;
    std::int64_t poll_interval = 5;
    std::vector<steam_login::GuardKind> allowed_guards;
    steam_login::GuardKind chosen_guard = steam_login::GuardKind::None;
    bool has_device_code = false;
    bool has_email_code = false;
    bool confirmation_mode = false;   // Steam offered approve-on-phone for this session
    bool email_confirmation_mode = false;  // Steam offered approve-via-email-link

    core::Account account;
    std::string status;
    std::string error;
    std::string guard_note;           // inline guard feedback that doesn't abort the flow
    bool busy = false;                // a foreground request (sign in / submit) is in flight
    bool poll_running = false;        // the token poll worker is active
    bool auto_guard_submitted = false;

    void reset() { *this = {}; }
};

widgets::StepperStep::State step_state(int step, int active_step) {
    if (step < active_step) return widgets::StepperStep::State::Done;
    if (step == active_step) return widgets::StepperStep::State::Active;
    return widgets::StepperStep::State::Pending;
}

bool poll_aborted(LoginWizard* w) {
    return w->phase == LoginWizard::Phase::Done ||
           w->phase == LoginWizard::Phase::Error ||
           w->phase == LoginWizard::Phase::Credentials;
}

// Polls PollAuthSessionStatus until tokens arrive (the session is confirmed by either a
// submitted Guard code or an approval in the Steam Mobile app), then registers the community
// session and merges the account into the vault. Safe to call every frame: launches at most once.
void launch_poll_worker(LoginWizard* w, app::AppState& state) {
    if (w->poll_running) return;
    w->poll_running = true;

    std::string cid = w->client_id;
    std::string rid = w->request_id;
    auto interval = w->poll_interval;
    auto sid = w->steam_id;

    app::job_pump::submit([cid, rid, interval, sid, w, &state] {
        std::string local_cid = cid;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

        while (std::chrono::steady_clock::now() < deadline) {
            auto poll = steam_login::poll_session(local_cid, rid);
            if (!poll.ok) {
                std::lock_guard lk(state.job_mutex);
                state.completed_jobs.push_back({"", [w, e = poll.error] {
                    if (poll_aborted(w)) return;
                    w->poll_running = false;
                    w->busy = false;
                    w->error = e;
                    w->phase = LoginWizard::Phase::Error;
                }});
                return;
            }
            if (poll.finished) {
                auto expiry = steam_login::jwt_expiry(poll.access_token);
                auto login_secure = crypto::make_secure(
                    steam_login::make_steam_login_secure(sid, poll.access_token));

                // Register the session in Steam's community session table so the
                // new account can accept confirmations without an auto-relogin first.
                // sess_id must be the sessionid we store: mobileconf sends it back as
                // a cookie and Steam matches it to this registered session.
                std::string sess_id = crypto::random_session_id();
                std::string registered_cookie;
                if (steam_login::transfer_login(sid, poll.refresh_token,
                                                 sess_id, registered_cookie)) {
                    login_secure = crypto::make_secure(registered_cookie);
                } else {
                    SAM_LOG_WARN("add_account: transfer_login failed for '{}'; "
                                 "mobileconf writes will need a relogin",
                                 w->account.login);
                }

                std::lock_guard lk(state.job_mutex);
                state.completed_jobs.push_back({"", [w, &state,
                        at = std::move(poll.access_token),
                        rt = std::move(poll.refresh_token),
                        expiry, ls = std::move(login_secure),
                        sess_id = std::move(sess_id)] {
                    if (poll_aborted(w)) return;

                    // Merge by steam_id or login to avoid duplicates when re-logging
                    // into an already-imported account (e.g. maFile with different casing).
                    core::Account* existing = core::store::find_existing_account(
                        state.vault, w->account.steam_id_64, w->account.login);

                    std::string id_for_refresh;
                    if (existing) {
                        existing->password         = w->account.password;
                        existing->steam_id_64      = w->account.steam_id_64;
                        existing->access_token     = at;
                        existing->refresh_token    = rt;
                        existing->access_token_expires = expiry;
                        existing->steam_login_secure   = ls;
                        // Must match the sessionid bound to ls, or mobileconf rejects the cookie.
                        existing->session_id = sess_id;
                        existing->last_login_unix = now_seconds();
                        id_for_refresh = existing->id;
                        // Carry over a supplied shared_secret, but don't clobber a
                        // previously-imported maFile when Full Login is re-run without one.
                        if (w->account.sda.has_value() &&
                            !w->account.sda->shared_secret.empty()) {
                            if (!existing->sda.has_value()) {
                                existing->sda = w->account.sda;
                            } else {
                                existing->sda->shared_secret =
                                    w->account.sda->shared_secret;
                                if (existing->sda->account_name.empty())
                                    existing->sda->account_name =
                                        w->account.sda->account_name;
                            }
                        }
                        w->status = "Existing account updated with fresh tokens.";
                    } else {
                        w->account.id = add_account_detail::generate_ulid();
                        w->account.access_token = at;
                        w->account.refresh_token = rt;
                        w->account.access_token_expires = expiry;
                        w->account.steam_login_secure = ls;
                        w->account.session_id = sess_id;
                        w->account.created_unix = now_seconds();
                        w->account.last_login_unix = now_seconds();
                        id_for_refresh = w->account.id;
                        state.vault.accounts.push_back(std::move(w->account));
                        w->status = "Login successful.";
                    }
                    state.vault_dirty = true;
                    state.save_vault_if_dirty();

                    if (!id_for_refresh.empty()) {
                        state.refresh_single_account(id_for_refresh);
                    }

                    w->poll_running = false;
                    w->busy = false;
                    w->phase = LoginWizard::Phase::Done;
                }});
                return;
            }
            if (!poll.new_client_id.empty()) {
                local_cid = poll.new_client_id;
            }
            std::this_thread::sleep_for(
                std::chrono::seconds(interval > 0 ? interval : 5));
        }

        std::lock_guard lk(state.job_mutex);
        state.completed_jobs.push_back({"", [w] {
            if (poll_aborted(w)) return;
            w->poll_running = false;
            w->busy = false;
            w->error = "Login timed out after 2 minutes.";
            w->phase = LoginWizard::Phase::Error;
        }});
    });
}

}  // namespace

namespace add_account_detail {

void draw_full_login(app::AppState& state) {
    static LoginWizard wiz;
    auto* w = &wiz;

    // Background refresh flagged this account for re-login; prefill username once, clear signal.
    if (state.pending_relogin_login.has_value() &&
        wiz.phase == LoginWizard::Phase::Credentials) {
        const auto& login = *state.pending_relogin_login;
        std::snprintf(wiz.username.data(), wiz.username.size(), "%s", login.c_str());
        wiz.status = "Re-login required: refresh_token expired";
        state.pending_relogin_login.reset();
    }

    int active_step = 0;
    switch (wiz.phase) {
        case LoginWizard::Phase::Credentials: active_step = 0; break;
        case LoginWizard::Phase::GuardWait:   active_step = 1; break;
        case LoginWizard::Phase::Polling:     active_step = 2; break;
        case LoginWizard::Phase::Done:        active_step = 3; break;
        case LoginWizard::Phase::Error:       active_step = -1; break;
    }

    std::vector<widgets::StepperStep> steps{
        {"Credentials", step_state(0, active_step)},
        {"Steam Guard", step_state(1, active_step)},
        {"Tokens",      step_state(2, active_step)},
        {"Done",        step_state(3, active_step)},
    };
    if (wiz.phase == LoginWizard::Phase::Error) {
        for (auto& s : steps) {
            if (s.state == widgets::StepperStep::State::Active)
                s.state = widgets::StepperStep::State::Failed;
        }
    }
    widgets::draw_stepper(steps);

    ImGui::SameLine();
    ImGui::BeginGroup();

    if (wiz.phase == LoginWizard::Phase::Credentials) {
        ImGui::SetNextItemWidth(280.0F);
        ImGui::InputText("Username", wiz.username.data(), wiz.username.size());
        hover_tooltip("Steam account name (lowercase username, not your persona).");
        widgets::draw_password_field("Password##login", wiz.password, false, 280.0F);
        hover_tooltip("Sent once to Steam's login endpoint to begin a mobile-confirmation "
                      "session. Stored encrypted in the vault on success.");
        widgets::draw_password_field("Shared secret (optional)##login-sda",
                                      wiz.shared_secret, false, 280.0F);
        hover_tooltip("Base64 shared_secret from your maFile. Optional: when present we "
                      "auto-generate the Steam Guard code and skip the manual code prompt.");
        ImGui::Spacing();

        const bool can_submit = wiz.username[0] != 0 && !wiz.password.empty() && !wiz.busy;
        ImGui::BeginDisabled(!can_submit);
        if (action_button("Sign in", ImVec2(120, 0))) {
            wiz.busy = true;
            wiz.status = "Requesting RSA key...";

            // Stash shared_secret on the pending account so begin_session can auto-submit
            // the Guard code and so it persists alongside the tokens on success.
            if (!wiz.shared_secret.empty()) {
                core::SteamGuardAccount g;
                g.account_name = wiz.username.data();
                g.shared_secret = wiz.shared_secret;
                wiz.account.sda = std::move(g);
            } else {
                wiz.account.sda.reset();
            }

            std::string user(wiz.username.data());
            auto pw = crypto::make_secure(wiz.password);

            app::job_pump::submit([user, pw, w, &state] {
                steam_login::MobileLogin login;
                login.username = user;
                login.password = pw;
                auto result = steam_login::begin_session(login);

                std::lock_guard lk(state.job_mutex);
                state.completed_jobs.push_back({"", [w, user, pw, r = std::move(result)] {
                    w->busy = false;
                    if (!r.ok) {
                        w->error = r.error;
                        w->phase = LoginWizard::Phase::Error;
                        return;
                    }
                    w->client_id = r.client_id;
                    w->request_id = r.request_id;
                    w->steam_id = r.steam_id;
                    w->poll_interval = r.interval_seconds > 0 ? r.interval_seconds : 5;
                    w->allowed_guards = r.allowed_confirmations;

                    w->account.login = user;
                    w->account.password = pw;
                    w->account.steam_id_64 = r.steam_id;

                    w->has_device_code = false;
                    w->has_email_code = false;
                    w->confirmation_mode = false;
                    w->email_confirmation_mode = false;
                    for (auto k : w->allowed_guards) {
                        if (k == steam_login::GuardKind::DeviceCode)
                            w->has_device_code = true;
                        else if (k == steam_login::GuardKind::EmailCode)
                            w->has_email_code = true;
                        else if (k == steam_login::GuardKind::DeviceConfirmation)
                            w->confirmation_mode = true;
                        else if (k == steam_login::GuardKind::EmailConfirmation)
                            w->email_confirmation_mode = true;
                    }
                    // The code kind we'd submit if the user chooses to type one.
                    w->chosen_guard = w->has_device_code ? steam_login::GuardKind::DeviceCode
                                    : w->has_email_code  ? steam_login::GuardKind::EmailCode
                                                         : steam_login::GuardKind::None;

                    if (w->confirmation_mode) {
                        w->phase = LoginWizard::Phase::GuardWait;
                        w->status = "Waiting for approval in your Steam Mobile app...";
                    } else if (w->email_confirmation_mode) {
                        // Approve-by-email-link: no code to submit, the poll worker
                        // (started in GuardWait) picks up the session once approved.
                        w->phase = LoginWizard::Phase::GuardWait;
                        w->status = "Waiting for email approval...";
                    } else if (w->chosen_guard != steam_login::GuardKind::None) {
                        w->phase = LoginWizard::Phase::GuardWait;
                        w->status = "Waiting for Steam Guard code...";
                    } else {
                        w->phase = LoginWizard::Phase::Polling;
                        w->status = "Polling for session tokens...";
                    }
                }});
            });
        }
        ImGui::EndDisabled();

    } else if (wiz.phase == LoginWizard::Phase::GuardWait) {
        // A confirmation prompt (mobile-app approval or an email link) means
        // PollAuthSessionStatus returns the tokens once it's approved, no separate RPC
        // needed, so poll immediately. The user can approve there or type a code below;
        // whichever lands first finishes the login.
        const bool conf_active = wiz.confirmation_mode || wiz.email_confirmation_mode;
        if (conf_active) {
            launch_poll_worker(w, state);
        }

        // With a shared_secret and an accepted DeviceCode, generate the TOTP and submit on the
        // first frame so a Guard-enrolled account never needs the manual prompt.
        const bool can_auto_submit = !wiz.auto_guard_submitted &&
            wiz.has_device_code &&
            wiz.account.sda.has_value() &&
            !wiz.account.sda->shared_secret.empty();

        if (can_auto_submit) {
            wiz.auto_guard_submitted = true;
            wiz.busy = true;
            wiz.status = "Generating Steam Guard code from shared_secret...";

            const std::string code = sam::sda::generate_code_now(
                wiz.account.sda->shared_secret);
            std::snprintf(wiz.guard_code.data(), wiz.guard_code.size(), "%s", code.c_str());

            std::string cid = wiz.client_id;
            std::uint64_t sid = wiz.steam_id;
            const bool conf = conf_active;

            app::job_pump::submit([code, cid, sid, conf, w, &state] {
                std::string err;
                bool ok = steam_login::submit_guard_code(
                    cid, sid, code, steam_login::GuardKind::DeviceCode, &err);

                std::lock_guard lk(state.job_mutex);
                state.completed_jobs.push_back({"", [w, &state, ok, err, conf] {
                    w->busy = false;
                    if (!ok) {
                        // Non-fatal: in confirmation mode the phone approval is still open;
                        // auto_guard_submitted stays true so we don't resubmit the stale code.
                        w->guard_note = "Auto-submitted code rejected: " + err;
                        return;
                    }
                    if (!conf) {
                        w->phase = LoginWizard::Phase::Polling;
                        w->status = "Polling for session tokens...";
                    }
                    // In confirmation mode the poll worker is already running and will pick up
                    // the now-confirmed session.
                }});
            });
        }

        if (wiz.confirmation_mode) {
            ImGui::TextWrapped("Approve the sign-in in your Steam Mobile app.");
            if (wiz.has_device_code || wiz.has_email_code) {
                ImGui::TextWrapped("Or enter a Steam Guard code below.");
            }
        } else if (wiz.email_confirmation_mode) {
            ImGui::TextWrapped("Check your email and approve the sign-in.");
            if (wiz.has_device_code || wiz.has_email_code) {
                ImGui::TextWrapped("Or enter the Steam Guard code below.");
            }
        } else if (wiz.chosen_guard == steam_login::GuardKind::EmailCode) {
            ImGui::TextWrapped("A Steam Guard code was sent to your email.");
        } else if (can_auto_submit || wiz.auto_guard_submitted) {
            ImGui::TextWrapped("Submitting Steam Guard code from shared_secret...");
        } else {
            ImGui::TextWrapped("Enter the code from your Steam Mobile authenticator.");
        }

        const bool show_code_field = wiz.has_device_code || wiz.has_email_code;
        if (show_code_field) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(120.0F);
            ImGui::InputText("Guard code", wiz.guard_code.data(), wiz.guard_code.size());
            hover_tooltip("The 5-character Steam Guard code (from email or the official mobile app).");
            ImGui::Spacing();

            const bool can_submit = wiz.guard_code[0] != 0 && !wiz.busy;
            ImGui::BeginDisabled(!can_submit);
            if (action_button("Submit code", ImVec2(120, 0))) {
                wiz.busy = true;
                wiz.status = "Submitting guard code...";
                wiz.guard_note.clear();

                std::string code(wiz.guard_code.data());
                std::string cid = wiz.client_id;
                std::uint64_t sid = wiz.steam_id;
                auto kind = wiz.chosen_guard;
                const bool conf = conf_active;

                app::job_pump::submit([code, cid, sid, kind, conf, w, &state] {
                    std::string err;
                    bool ok = steam_login::submit_guard_code(cid, sid, code, kind, &err);

                    std::lock_guard lk(state.job_mutex);
                    state.completed_jobs.push_back({"", [w, &state, ok, err, conf] {
                        w->busy = false;
                        if (!ok) {
                            if (conf) {
                                // Phone approval is still viable, so stay on the page.
                                w->guard_note = "Code rejected: " + err;
                            } else {
                                w->error = "Guard code rejected: " + err;
                                w->phase = LoginWizard::Phase::Error;
                            }
                            return;
                        }
                        if (conf) {
                            launch_poll_worker(w, state);  // no-op if already polling
                        } else {
                            w->phase = LoginWizard::Phase::Polling;
                            w->status = "Polling for session tokens...";
                        }
                    }});
                });
            }
            ImGui::EndDisabled();
        }

        if (!wiz.guard_note.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", wiz.guard_note.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        if (action_button("Cancel", ImVec2(120, 0))) {
            wiz.busy = false;
            wiz.error = "Login cancelled.";
            wiz.phase = LoginWizard::Phase::Error;
        }

    } else if (wiz.phase == LoginWizard::Phase::Polling) {
        launch_poll_worker(w, state);

        ImGui::TextWrapped("Waiting for Steam to confirm the session...");
        ImGui::Spacing();
        // The poll worker bails on its next post-back once the phase leaves Polling/GuardWait.
        if (action_button("Cancel", ImVec2(120, 0))) {
            wiz.busy = false;
            wiz.error = "Login cancelled.";
            wiz.phase = LoginWizard::Phase::Error;
        }

    } else if (wiz.phase == LoginWizard::Phase::Done) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::TextWrapped("Account authenticated and saved to vault.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (action_button("Go to Accounts", ImVec2(160, 0))) {
            wiz.reset();
            state.current_screen = app::Screen::Accounts;
        }

    } else if (wiz.phase == LoginWizard::Phase::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("%s", wiz.error.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (action_button("Try again", ImVec2(120, 0))) {
            wiz.reset();
        }
    }

    if (!wiz.status.empty() && (wiz.busy || wiz.poll_running)) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextUnformatted(wiz.status.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndGroup();
}

}  // namespace add_account_detail

}  // namespace sam::ui::screens
