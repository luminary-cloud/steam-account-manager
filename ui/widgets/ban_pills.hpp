#pragma once

#include "core/account_store/account.hpp"

namespace sam::ui::widgets {

struct BanPillsOptions {
    bool show_vac       = true;
    bool show_game      = true;
    bool show_trade     = true;
    bool show_community = true;
    bool show_vac_live  = false;
    bool vac_live       = false;
};

void draw_ban_pills(const core::BanStatus& bans, const BanPillsOptions& opts);

}  // namespace sam::ui::widgets
