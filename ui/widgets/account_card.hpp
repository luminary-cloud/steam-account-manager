#pragma once

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

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

// Draws one account card. Returns a non-None action if the user clicked one of
// the card buttons. `width` is the rendered card width; the grid in
// accounts_screen.cpp stretches this to fill the available row.
CardAction draw_account_card(app::AppState& state, core::Account& account, float width);

}  // namespace sam::ui::widgets
