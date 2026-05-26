#pragma once

#include <cstddef>
#include <span>

#include "app/app_state.hpp"
#include "ui/widgets/account_card.hpp"

namespace sam::ui::screens {

struct ListViewResult {
    widgets::CardAction action = widgets::CardAction::None;
    std::string action_account_id;
};

ListViewResult draw_list_body(app::AppState& state,
                              std::span<const std::size_t> visible);

void draw_new_group_modal(app::AppState& state);

}  // namespace sam::ui::screens
