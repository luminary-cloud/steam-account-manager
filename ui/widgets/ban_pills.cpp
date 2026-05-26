#include "ui/widgets/ban_pills.hpp"

#include <imgui.h>

#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::widgets {

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
    };

    if (opts.show_vac)       pill("VAC",   red,    b.vac_banned);
    if (opts.show_game)      pill("GAME",  red,    b.game_ban_count > 0);
    if (opts.show_trade)     pill("TRADE", yellow, b.economy_ban == "banned" || b.economy_ban == "probation");
    if (opts.show_community) pill("COMM",  yellow, b.community_banned);
    if (opts.show_vac_live && opts.vac_live) pill("VAC-LIVE", red, true);
}

}  // namespace sam::ui::widgets
