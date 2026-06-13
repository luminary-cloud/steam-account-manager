#pragma once

#include <cstddef>

#include <imgui.h>

namespace sam::ui::widgets {

enum class MarkerKind {
    UnreadEvent,
    Cooldown,
    WeeklyDrop,
};

inline constexpr float kMarkerSize = 14.0F;

// `count` > 1 is appended as a numeric suffix (UnreadEvent only).
void draw_marker(ImDrawList* dl, ImVec2 center, MarkerKind kind, std::size_t count = 1);

}  // namespace sam::ui::widgets
