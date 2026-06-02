#pragma once

#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Draws a small coloured dot reflecting the trust label. Click cycles to the
// next state if `editable`. Returns true if the value changed.
bool draw_trust_badge(core::TrustLabel& trust, bool editable = false, float radius = 6.0F);

// Draws a small amber "NFA" pill whose right edge is at `right_x` (screen X),
// vertically centred on `center_y`. Marks NFA accounts beside the trust dot.
void draw_nfa_pill(float right_x, float center_y);

}  // namespace sam::ui::widgets
