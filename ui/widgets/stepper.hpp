#pragma once

#include <string>
#include <vector>

namespace sam::ui::widgets {

struct StepperStep {
    std::string title;
    enum class State { Pending, Active, Done, Failed } state = State::Pending;
};

void draw_stepper(const std::vector<StepperStep>& steps);

}  // namespace sam::ui::widgets
