#pragma once

#include <array>
#include <string>

#include "app/app_state.hpp"

namespace sam::ui::widgets {

struct AddSdaDialogState {
    enum class Step {
        Idle,
        Working,          // a network call is in flight; working_text describes it
        ShowRcode,        // displaying the revocation code, waiting for user ack
        AwaitActivation,  // user is entering the activation code
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
    bool resync_attempted = false;
    bool bad_code_hint = false;
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
};

// Open the Add wizard. If the account already has a half-linked SDA (sda set but
// fully_enrolled == false) the dialog jumps straight to the activation step so
// the user can finish what they started.
void request_add_sda(app::AppState& app, AddSdaDialogState& state, std::string account_id);

void request_remove_sda(RemoveSdaDialogState& state, std::string account_id);

void draw_add_sda_modal(app::AppState& app, AddSdaDialogState& state);
void draw_remove_sda_modal(app::AppState& app, RemoveSdaDialogState& state);

}  // namespace sam::ui::widgets
