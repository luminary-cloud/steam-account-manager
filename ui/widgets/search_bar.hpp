#pragma once

#include <string>

namespace sam::ui::widgets {

// Draws a search input plus an optional clear button. Returns true if the
// query changed since the last call.
bool draw_search_bar(std::string& query, float width = -1.0F);

}  // namespace sam::ui::widgets
