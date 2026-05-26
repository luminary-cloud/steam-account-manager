#pragma once

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"
#include "ui/widgets/account_card.hpp"

namespace sam::ui::widgets {

CardAction draw_account_panel(app::AppState& state, core::Account& account);

}  // namespace sam::ui::widgets
