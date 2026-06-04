#pragma once

#include <cstddef>

#include <imgui.h>

namespace sam::ui::widgets {

enum class MarkerKind {
    UnreadEvent,
    Cooldown,
    WeeklyDrop,
};

// Diameter in pixels for the unread/cooldown markers.
inline constexpr float kMarkerSize = 14.0F;

// Draws a coloured circle at `center`: a centred "!" glyph for UnreadEvent
// (danger red) and Cooldown (warning orange), or a white checkmark for
// WeeklyDrop (success green), so each reads as visually distinct from the trust
// dot at the row start. `count`, when > 1, is appended as a small numeric suffix
// beside the badge (UnreadEvent only).
void draw_marker(ImDrawList* dl, ImVec2 center, MarkerKind kind, std::size_t count = 1);

}  // namespace sam::ui::widgets
