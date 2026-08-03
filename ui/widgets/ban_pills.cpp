#include "ui/widgets/ban_pills.hpp"

#include <cstdint>
#include <string>

#include <imgui.h>

#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::widgets {

namespace {

std::string last_ban_line(const core::BanStatus& b) {
    if (b.days_since_last_ban <= 0) return {};
    const std::int64_t base = b.last_refreshed_unix > 0 ? b.last_refreshed_unix
                                                        : now_seconds();
    const std::int64_t when =
        base - static_cast<std::int64_t>(b.days_since_last_ban) * 86400;
    return "\nMost recent ban (VAC or game): " + std::to_string(b.days_since_last_ban) +
           " days ago, around " + format_date(when) + ".";
}

std::string checked_line(const core::BanStatus& b) {
    if (b.last_refreshed_unix <= 0)
        return "\nNo ban data yet - hit Refresh on this account.";
    return "\nChecked " + format_relative(b.last_refreshed_unix) + ".";
}

std::string vac_tip(const core::BanStatus& b) {
    std::string tip = b.vac_banned ? "VAC banned." : "No VAC bans.";
    if (b.vac_ban_count > 0) tip += "\nVAC bans: " + std::to_string(b.vac_ban_count);
    if (b.vac_banned) tip += last_ban_line(b);
    return tip + checked_line(b);
}

std::string game_tip(const core::BanStatus& b) {
    std::string tip = b.game_ban_count > 0
        ? "Game bans: " + std::to_string(b.game_ban_count) +
          " (bans issued by a game's developer, not by VAC)."
        : std::string("No game bans.");
    if (b.game_ban_count > 0) tip += last_ban_line(b);
    return tip + checked_line(b);
}

std::string trade_tip(const core::BanStatus& b) {
    std::string tip;
    if (b.economy_ban == "banned") {
        tip = "Trade banned: this account can't trade or use the Community Market.";
    } else if (b.economy_ban == "probation") {
        tip = "Trade probation: trading is restricted.";
    } else if (b.economy_ban.empty() || b.economy_ban == "none") {
        tip = "No trade ban.";
    } else {
        tip = "Trade status: " + b.economy_ban + ".";
    }
    return tip + checked_line(b);
}

std::string community_tip(const core::BanStatus& b) {
    std::string tip = b.community_banned
        ? std::string("Community banned: no forums, Workshop, groups or reviews.")
        : std::string("No community ban.");
    return tip + checked_line(b);
}

}  // namespace

void draw_ban_pills(const core::BanStatus& b, const BanPillsOptions& opts) {
    const ImVec4 red = theme::danger();
    const ImVec4 yellow = theme::warning();

    int n = 0;
    if (opts.show_vac)       ++n;
    if (opts.show_game)      ++n;
    if (opts.show_trade)     ++n;
    if (opts.show_community) ++n;
    if (opts.show_vac_live && opts.vac_live) ++n;
    if (n == 0) return;

    const float avail = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float pill_w = (avail - spacing * static_cast<float>(n - 1)) /
                         static_cast<float>(n);

    bool any = false;

    auto pill = [&](const char* label, const ImVec4& color, bool on) {
        if (any) ImGui::SameLine();
        draw_pill(label, color, on, pill_w);
        any = true;
        return ImGui::IsItemHovered();
    };

    if (opts.show_vac && pill("VAC", red, b.vac_banned)) {
        set_tooltip("%s", vac_tip(b).c_str());
    }
    if (opts.show_game && pill("GAME", red, b.game_ban_count > 0)) {
        set_tooltip("%s", game_tip(b).c_str());
    }
    if (opts.show_trade &&
        pill("TRADE", yellow,
             b.economy_ban == "banned" || b.economy_ban == "probation")) {
        set_tooltip("%s", trade_tip(b).c_str());
    }
    if (opts.show_community && pill("COMM", yellow, b.community_banned)) {
        set_tooltip("%s", community_tip(b).c_str());
    }
    if (opts.show_vac_live && opts.vac_live && pill("VAC-LIVE", red, true)) {
        set_tooltip("VAC ban reported by the CS2 Game Coordinator. Usually shows "
                    "before the Web API's VAC flag.");
    }
}

}  // namespace sam::ui::widgets
