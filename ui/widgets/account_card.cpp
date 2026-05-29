#include "ui/widgets/account_card.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/sda/totp.hpp"
#include "core/time_aligner.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/account_context_menu.hpp"
#include "ui/widgets/avatar.hpp"
#include "ui/widgets/ban_pills.hpp"
#include "ui/widgets/rank_image.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/status_markers.hpp"
#include "ui/widgets/tag_chip.hpp"
#include "ui/widgets/trust_badge.hpp"

namespace sam::ui::widgets {

namespace {

constexpr float kCardHeight = kAccountCardHeight;
constexpr float kButtonRowH = 40.0F;
constexpr float kButtonRowReserve = 50.0F;   // button height + gap above

std::string format_with_commas(int value) {
    char raw[16];
    std::snprintf(raw, sizeof(raw), "%d", value);
    std::string s(raw);
    const bool negative = !s.empty() && s.front() == '-';
    const std::size_t start = negative ? 1 : 0;
    std::size_t n = s.size() - start;
    if (n <= 3) return s;
    std::string out;
    out.reserve(s.size() + (n - 1) / 3);
    if (negative) out.push_back('-');
    const std::size_t lead = n % 3 == 0 ? 3 : n % 3;
    out.append(s, start, lead);
    for (std::size_t i = start + lead; i < s.size(); i += 3) {
        out.push_back(',');
        out.append(s, i, 3);
    }
    return out;
}

// Notes are shown inline on the card's subtitle line. Flatten newlines to
// spaces and clip to the available width with an ellipsis so a long or
// multi-line note can't push the rest of the card's layout down.
std::string fit_notes_single_line(const std::string& notes, float max_w) {
    if (max_w <= 0.0F) return {};
    std::string flat;
    flat.reserve(notes.size());
    for (char c : notes) {
        flat.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
    }
    if (ImGui::CalcTextSize(flat.c_str()).x <= max_w) return flat;
    const float ellipsis_w = ImGui::CalcTextSize("...").x;
    std::string out = flat;
    while (!out.empty()) {
        while (!out.empty()) {
            const unsigned char uc = static_cast<unsigned char>(out.back());
            out.pop_back();
            if ((uc & 0xC0) != 0x80) break;
        }
        if (out.empty()) break;
        if (ImGui::CalcTextSize(out.c_str()).x + ellipsis_w <= max_w) break;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += "...";
    return out;
}

}  // namespace

CardAction draw_account_card(app::AppState& state, core::Account& a, float width) {
    CardAction action = CardAction::None;

    const bool selected = state.selection_mode && state.is_selected(a.id);

    ImGui::PushID(a.id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel());
    ImGui::PushStyleColor(ImGuiCol_Border,
                          selected ? theme::accent() : theme::border());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));

    ImGui::BeginChild("##card", ImVec2(width, kCardHeight), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);

    // Header strip.
    const ImVec2 avatar_origin = ImGui::GetCursorScreenPos();
    draw_avatar(a, 48.0F);
    if (state.settings.notifications.enabled &&
        state.settings.notifications.surface_in_card) {
        const auto unread = state.notifications.unacked_count_for(a.id);
        if (unread > 0) {
            draw_marker(ImGui::GetWindowDrawList(),
                        ImVec2(avatar_origin.x + 48.0F - 2.0F,
                                avatar_origin.y + 2.0F),
                        MarkerKind::UnreadEvent, unread);
        }
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (!a.web.persona_name.empty()) {
        ImGui::TextUnformatted(a.web.persona_name.c_str());
    } else {
        // No persona to fall back to: the heading itself is the login.
        draw_login_text(state, a);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
    draw_login_text(state, a);
    if (!a.web.country_code.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("·");
        ImGui::SameLine();
        std::string cc;
        cc.reserve(a.web.country_code.size());
        for (char c : a.web.country_code)
            cc.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        ImGui::TextDisabled("%s", cc.c_str());
    }
    if (!state.settings.hide_notes && !a.notes.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("·");
        ImGui::SameLine();
        const std::string notes_utf8 = to_utf8(a.notes);
        const std::string notes_line =
            fit_notes_single_line(notes_utf8, ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("%s", notes_line.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    ImGui::SameLine();
    {
        const float right_pad = 16.0F;
        const float badge_w = 14.0F;
        ImGui::SetCursorPosX(ImGui::GetWindowSize().x - badge_w - right_pad);
        if (draw_trust_badge(a.trust, true)) {
            state.vault_dirty = true;
            state.save_vault_if_dirty();
        }
    }

    ImGui::Spacing();

    // Bans + VAC-Live (each pill is gated by its own settings toggle).
    {
        BanPillsOptions opts;
        opts.show_vac       = state.settings.info.show_vac;
        opts.show_game      = state.settings.info.show_game_ban;
        opts.show_trade     = state.settings.info.show_trade_ban;
        opts.show_community = state.settings.info.show_community_ban;
        opts.show_vac_live  = state.settings.info.show_vac_live;
        opts.vac_live       = a.cs2.vac_live;
        if (opts.show_vac || opts.show_game || opts.show_trade ||
            opts.show_community || (opts.show_vac_live && opts.vac_live)) {
            draw_ban_pills(a.bans, opts);
        }
    }

    ImGui::Spacing();

    // Stats row: Steam Level · Games · CS2 Rank · Playtime · Prime.
    {
        bool any = false;
        auto sep = [&]() {
            if (any) { ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine(); }
            any = true;
        };
        if (state.settings.info.show_steam_level && a.web.steam_level > 0) {
            sep();
            ImGui::TextDisabled("Level");
            ImGui::SameLine();
            ImGui::Text("%d", a.web.steam_level);
        }
        if (state.settings.info.show_owned_games && a.web.owned_games_count >= 0) {
            sep();
            ImGui::TextDisabled("Games");
            ImGui::SameLine();
            ImGui::Text("%d", a.web.owned_games_count);
        }
        if (a.cs2.cs2_player_level >= 0) {
            sep();
            ImGui::TextDisabled("CS2 Rank");
            ImGui::SameLine();
            ImGui::Text("%d", a.cs2.cs2_player_level);
        }
        if (state.settings.info.show_owned_games && a.web.total_playtime_minutes > 0) {
            sep();
            ImGui::TextDisabled("Playtime");
            ImGui::SameLine();
            ImGui::Text("%lld h",
                static_cast<long long>(a.web.total_playtime_minutes / 60));
        }
        if (state.settings.info.show_prime && a.cs2.prime_status) {
            sep();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
            ImGui::TextUnformatted("Prime");
            ImGui::PopStyleColor();
        }
        if (any) ImGui::NewLine();
    }

    // Tags.
    if (!a.tag_ids.empty()) {
        for (const auto& tag_id : a.tag_ids) {
            for (const auto& t : state.vault.tags) {
                if (t.id == tag_id) {
                    draw_tag_chip(t.name, t.color_rgba);
                    ImGui::SameLine();
                    break;
                }
            }
        }
        ImGui::NewLine();
    }

    // Rank badges: Premier tier image (with ELO overlay or "---" placeholder)
    // and Wingman rank icon (rank 0 = unranked placeholder), each with a
    // "Wins: N" label above. Horizontally arranged with equal 3-way gaps:
    // [gap][premier][gap][wingman][gap]. Vertically centered between the
    // content above and the cooldown / button row below.
    {
        constexpr float kPremierH = 26.0F;
        constexpr float kWingmanH = 26.0F;
        constexpr float kBadgeH   = 26.0F;
        constexpr float kWinsGap  = 2.0F;

        const bool show_premier_slot = state.settings.info.show_premier;
        const bool show_wingman_slot = state.settings.info.show_wingman;

        const rank_image::TexEntry* premier_e = nullptr;
        if (show_premier_slot) {
            const int bracket = a.cs2.premier_rating > 0
                ? premier_bracket_for_rating(a.cs2.premier_rating)
                : 0;
            premier_e = rank_image::premier(bracket);
        }
        const rank_image::TexEntry* wingman_e = nullptr;
        if (show_wingman_slot) {
            const int rank = a.cs2.wingman_rank > 0 ? a.cs2.wingman_rank : 0;
            wingman_e = rank_image::wingman(rank);
        }

        const bool draw_premier = premier_e && premier_e->srv && premier_e->h > 0;
        const bool draw_wingman = wingman_e && wingman_e->srv && wingman_e->h > 0;

        if (draw_premier || draw_wingman) {
            const float premier_w = draw_premier
                ? kPremierH * (static_cast<float>(premier_e->w) / static_cast<float>(premier_e->h))
                : 0.0F;
            const float wingman_w = draw_wingman
                ? kWingmanH * (static_cast<float>(wingman_e->w) / static_cast<float>(wingman_e->h))
                : 0.0F;

            // Vertical: center between current flow position and the top of the
            // cooldown row (or the button row if no cooldown is showing).
            bool cd_active = false;
            if (state.settings.info.show_cooldown && a.cs2.cooldown_expires_unix > 0) {
                const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                cd_active = a.cs2.cooldown_expires_unix > now_s;
            }
            const float bottom_y = ImGui::GetWindowSize().y - kButtonRowReserve -
                                   (cd_active ? ImGui::GetTextLineHeightWithSpacing() : 0.0F);
            // The trailing NewLine after the stats row leaves the cursor a full
            // text-line (plus item spacing) below the visible content; subtract
            // that so the center is computed against the *actual* bottom of
            // the stats text.
            const float top_y    = std::max(0.0F,
                ImGui::GetCursorPosY() - ImGui::GetTextLineHeightWithSpacing());
            const float wins_h   = ImGui::GetTextLineHeight();
            const float row_h    = wins_h + kWinsGap + kBadgeH;
            const float center_y = (top_y + bottom_y) * 0.5F;
            const float row_y    = std::max(top_y, center_y - row_h * 0.5F);
            const float badge_y  = row_y + wins_h + kWinsGap;

            // Horizontal: equal 3-way gap when both icons show (left margin,
            // middle gap, right margin), 2-way when only one icon shows.
            const float inner_w  = ImGui::GetWindowSize().x -
                                   2.0F * ImGui::GetStyle().WindowPadding.x;
            const int n_gaps = (draw_premier && draw_wingman) ? 3 : 2;
            const float gap = std::max(0.0F,
                (inner_w - premier_w - wingman_w) / static_cast<float>(n_gaps));
            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x + gap, badge_y));

            auto* dl = ImGui::GetWindowDrawList();

            auto draw_wins_label = [&](const ImVec2& screen_pos, float badge_w, int wins) {
                char buf[32];
                if (wins >= 0) std::snprintf(buf, sizeof(buf), "Wins: %d", wins);
                else           std::snprintf(buf, sizeof(buf), "Wins: ---");
                const ImVec2 ts = ImGui::CalcTextSize(buf);
                const float tx = screen_pos.x + (badge_w - ts.x) * 0.5F;
                const float ty = screen_pos.y - kWinsGap - wins_h;
                const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                dl->AddText(ImVec2(tx, ty), col, buf);
            };

            if (draw_premier) {
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                const ImVec2 size(premier_w, kPremierH);
                ImGui::Dummy(size);
                dl->AddImage(reinterpret_cast<ImTextureID>(premier_e->srv),
                             cursor, ImVec2(cursor.x + size.x, cursor.y + size.y));

                draw_wins_label(cursor, premier_w, a.cs2.premier_wins);

                const std::string text = a.cs2.premier_rating > 0
                    ? format_with_commas(a.cs2.premier_rating)
                    : std::string("---");
                ImFont* font = fonts::badge();
                const float fsize = font->LegacySize;
                const ImVec2 ts = font->CalcTextSizeA(fsize, FLT_MAX, 0.0F, text.c_str());
                const float tx = cursor.x + size.x * 0.58F - ts.x * 0.5F;
                const float ty = cursor.y + (size.y - fsize) * 0.5F - 2.0F;
                const ImU32 col = a.cs2.premier_rating > 0
                    ? IM_COL32_WHITE
                    : IM_COL32(255, 255, 255, 140);
                dl->AddText(font, fsize, ImVec2(tx, ty), col, text.c_str());

                if (draw_wingman) {
                    ImGui::SameLine(0.0F, gap);
                }
            }

            if (draw_wingman) {
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                const ImVec2 size(wingman_w, kWingmanH);
                ImGui::Dummy(size);
                dl->AddImage(reinterpret_cast<ImTextureID>(wingman_e->srv),
                             cursor, ImVec2(cursor.x + size.x, cursor.y + size.y));

                draw_wins_label(cursor, wingman_w, a.cs2.wingman_wins);
            }
        }
    }

    // Cooldown indicator: pinned just above the action row so it can't
    // overlap with the buttons even when the stats above are tall.
    if (state.settings.info.show_cooldown && a.cs2.cooldown_expires_unix > 0) {
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (a.cs2.cooldown_expires_unix > now_s) {
            const auto remaining = a.cs2.cooldown_expires_unix - now_s;
            const float cd_y = ImGui::GetWindowSize().y - kButtonRowReserve -
                               ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetCursorPosY(cd_y);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            const char* reason = a.cs2.cooldown_reason.empty()
                ? "active" : a.cs2.cooldown_reason.c_str();
            if (remaining >= 86400) {
                ImGui::Text("%s · %lldd left",
                            reason, static_cast<long long>(remaining / 86400));
            } else if (remaining >= 3600) {
                ImGui::Text("%s · %lldh left",
                            reason, static_cast<long long>(remaining / 3600));
            } else {
                ImGui::Text("%s · %lldm left",
                            reason, static_cast<long long>(remaining / 60));
            }
            ImGui::PopStyleColor();
        }
    }

    // Action row pinned to the bottom of the card. In selection mode the row
    // is replaced by a single checkbox so the per-card actions can't fire
    // while bulk-selecting.
    if (state.selection_mode) {
        const float btn_y = ImGui::GetWindowSize().y - kButtonRowH;
        ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, btn_y));
        bool checked = selected;
        if (ImGui::Checkbox("Select", &checked)) {
            action = CardAction::ToggleSelect;
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        return action;
    }

    const int n_buttons = a.sda.has_value() ? 5 : 4;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float content_w = width - 2.0F * ImGui::GetStyle().WindowPadding.x;
    const float btn_w = (content_w - spacing * static_cast<float>(n_buttons - 1)) /
                        static_cast<float>(n_buttons);
    const float btn_y = ImGui::GetWindowSize().y - kButtonRowH;
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, btn_y));

    if (action_button("Login", ImVec2(btn_w, 0))) action = CardAction::Launch;
    ImGui::SameLine();
    if (a.sda.has_value()) {
        // Primary action is copy-to-clipboard; right-click opens the
        // dedicated Authenticator screen for users who want the visible
        // countdown.
        if (action_button("Code", ImVec2(btn_w, 0))) {
            action = CardAction::CopyCode;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) action = CardAction::OpenSda;
        hover_tooltip("Click to copy the current Steam Guard code. "
                      "Right-click to open the Authenticator tab.");
        ImGui::SameLine();
    }
    {
        const std::int64_t cooldown = state.refresh_cooldown_seconds(a.id);
        const bool refresh_busy = state.refreshing_ids.count(a.id) > 0;
        const bool refresh_disabled = refresh_busy || cooldown > 0;
        ImGui::BeginDisabled(refresh_disabled);
        if (action_button("Refresh", ImVec2(btn_w, 0))) action = CardAction::Refresh;
        ImGui::EndDisabled();
        // hover_tooltip uses ImGui::IsItemHovered() which is false on disabled
        // items by default; query with AllowWhenDisabled so the cooldown reason
        // is still visible.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (refresh_busy) {
                ImGui::SetTooltip("Refresh in progress...");
            } else if (cooldown > 0) {
                ImGui::SetTooltip("Wait %llds before refreshing again.",
                                  static_cast<long long>(cooldown));
            }
        }
    }
    ImGui::SameLine();
    if (action_button("Edit",    ImVec2(btn_w, 0))) action = CardAction::Edit;
    ImGui::SameLine();
    if (action_button("Remove",  ImVec2(btn_w, 0))) action = CardAction::Remove;

    if (!state.selection_mode) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        if (ImGui::BeginPopupContextWindow(
                "##acc-ctx",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            draw_account_context_menu(state, a);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::PopID();

    return action;
}

}  // namespace sam::ui::widgets
