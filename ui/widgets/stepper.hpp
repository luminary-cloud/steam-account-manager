#pragma once

#include <string>
#include <vector>

namespace sam::ui::widgets {

struct StepperStep {
    std::string title;
    enum class State { Pending, Active, Done, Failed } state = State::Pending;
};

// Draws a vertical stepper used by the Add Account → Full Login wizard.
void draw_stepper(const std::vector<StepperStep>& steps);

}  // namespace sam::ui::widgets
