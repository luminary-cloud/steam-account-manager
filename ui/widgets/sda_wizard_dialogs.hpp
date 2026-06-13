#pragma once

#include <array>
#include <string>

#include "app/app_state.hpp"

namespace sam::ui::widgets {

struct AddSdaDialogState {
    enum class Step {
        Idle,
        Working,          // network call in flight; working_text describes it
        ShowRcode,
        AwaitActivation,
        Done,
        Failed,
    };

    bool open = false;
    Step step = Step::Idle;
    std::string account_id;
    std::array<char, 16> activation_buf{};
    std::string error;
    std::string working_text;
    bool acknowledged_rcode = false;
    bool bad_code_hint = false;
    // Consumed in draw_*_modal so OpenPopup shares the modal's ID-stack scope
    // (the triggering button may sit inside a PushID).
    bool pending_open = false;
};

struct RemoveSdaDialogState {
    enum class Step {
        Idle,
        Confirm,
        Working,
        Done,
        Failed,
    };

    bool open = false;
    Step step = Step::Idle;
    std::string account_id;
    int scheme = 1;                 // 1 = revert to email guard, 2 = remove guard entirely
    std::array<char, 8> code_check{};
    std::string error;
    bool pending_open = false;
};

// A half-linked SDA (sda set but fully_enrolled == false) resumes at the
// activation step instead of calling AddAuthenticator again.
void request_add_sda(app::AppState& app, AddSdaDialogState& state, std::string account_id);

// Opens straight into "verify with Steam" mode: QueryStatus confirms the maFile
// is the live authenticator and marks it fully_enrolled, for maFiles that carry
// a stale fully_enrolled == false even though Steam Guard already works.
void request_verify_sda(app::AppState& app, AddSdaDialogState& state, std::string account_id);

void request_remove_sda(RemoveSdaDialogState& state, std::string account_id);

void draw_add_sda_modal(app::AppState& app, AddSdaDialogState& state);
void draw_remove_sda_modal(app::AppState& app, RemoveSdaDialogState& state);

}  // namespace sam::ui::widgets
