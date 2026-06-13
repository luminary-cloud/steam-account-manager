#pragma once

#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Click cycles to the next state when `editable`. Returns true if it changed.
bool draw_trust_badge(core::TrustLabel& trust, bool editable = false, float radius = 6.0F);

// Right edge at `right_x` (screen X), centred vertically on `center_y`.
void draw_nfa_pill(float right_x, float center_y);

}  // namespace sam::ui::widgets
