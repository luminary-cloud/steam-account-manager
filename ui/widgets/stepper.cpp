#include "ui/widgets/stepper.hpp"

#include <imgui.h>

#include "ui/theme.hpp"

namespace sam::ui::widgets {

void draw_stepper(const std::vector<StepperStep>& steps) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    constexpr float row_h = 36.0F;
    constexpr float radius = 9.0F;
    constexpr float text_offset = 28.0F;

    for (std::size_t i = 0; i < steps.size(); ++i) {
        const ImVec2 center{origin.x + radius + 2.0F, origin.y + i * row_h + radius + 2.0F};

        ImU32 fill = IM_COL32(80, 80, 80, 220);
        switch (steps[i].state) {
            case StepperStep::State::Pending: break;
            case StepperStep::State::Active:
                fill = ImGui::ColorConvertFloat4ToU32(theme::accent());
                break;
            case StepperStep::State::Done:
                fill = ImGui::ColorConvertFloat4ToU32(theme::success());
                break;
            case StepperStep::State::Failed:
                fill = ImGui::ColorConvertFloat4ToU32(theme::danger());
                break;
        }
        dl->AddCircleFilled(center, radius, fill, 24);

        if (i + 1 < steps.size()) {
            dl->AddLine(ImVec2(center.x, center.y + radius),
                        ImVec2(center.x, center.y + row_h - radius),
                        IM_COL32(255, 255, 255, 40), 2.0F);
        }

        const ImVec2 text_pos{origin.x + text_offset, origin.y + i * row_h + 6.0F};
        const ImU32 text_col = steps[i].state == StepperStep::State::Active
            ? ImGui::ColorConvertFloat4ToU32(theme::text())
            : ImGui::ColorConvertFloat4ToU32(theme::dim_text());
        dl->AddText(text_pos, text_col, steps[i].title.c_str());
    }
    ImGui::Dummy(ImVec2(220.0F, steps.size() * row_h));
}

}  // namespace sam::ui::widgets
