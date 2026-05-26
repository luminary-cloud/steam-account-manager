#include "ui/widgets/search_bar.hpp"

#include <array>

#include <imgui.h>

#include "ui/util.hpp"

namespace sam::ui::widgets {

bool draw_search_bar(std::string& query, float width) {
    if (width > 0.0F) ImGui::SetNextItemWidth(width);

    std::array<char, 256> buf{};
    std::snprintf(buf.data(), buf.size(), "%s", query.c_str());

    bool changed = false;
    if (ImGui::InputTextWithHint("##search", "Search login, persona, notes", buf.data(), buf.size())) {
        query = buf.data();
        changed = true;
    }

    if (!query.empty()) {
        ImGui::SameLine();
        if (sam::ui::action_button("clear")) {
            query.clear();
            changed = true;
        }
    }
    return changed;
}

}  // namespace sam::ui::widgets
