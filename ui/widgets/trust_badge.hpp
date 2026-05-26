#pragma once

#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Draws a small coloured dot reflecting the trust label. Click cycles to the
// next state if `editable`. Returns true if the value changed.
bool draw_trust_badge(core::TrustLabel& trust, bool editable = false, float radius = 6.0F);

}  // namespace sam::ui::widgets
