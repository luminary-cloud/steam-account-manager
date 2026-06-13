#pragma once

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

// Exposed so the grid's ImGuiListClipper in accounts_screen.cpp can seek by
// row. Sized so the rank badges clear the cooldown + weekly-drop strip even
// when both indicator lines show.
constexpr float kAccountCardHeight = 308.0F;

enum class CardAction {
    None,
    Launch,
    Reveal,
    Refresh,
    Edit,
    Remove,
    OpenSda,
    CopyCode,
    ToggleSelect,
};

CardAction draw_account_card(app::AppState& state, core::Account& account, float width);

}  // namespace sam::ui::widgets
