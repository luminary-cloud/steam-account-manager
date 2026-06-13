#include "ui/widgets/account_panel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "ui/fonts.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar.hpp"
#include "ui/widgets/ban_pills.hpp"
#include "ui/widgets/login_method_control.hpp"
#include "ui/widgets/rank_image.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/status_markers.hpp"
#include "ui/widgets/tag_chip.hpp"
#include "ui/widgets/trust_badge.hpp"

#include "core/account_store/account.hpp"
#include "core/account_store/ban_diff.hpp"
#include "core/account_store/ban_event.hpp"

namespace sam::ui::widgets {

namespace {

constexpr float kAvatarSize = 56.0F;
constexpr float kFooterReserved = 60.0F;
constexpr float kFooterGap = 8.0F;

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

std::string format_date(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    const auto t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string format_relative(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) return "never";
    const auto delta = now_seconds() - unix_seconds;
    if (delta < 0)        return "in the future";
    if (delta < 60)       return std::to_string(delta) + "s ago";
    if (delta < 3600)     return std::to_string(delta / 60) + "m ago";
    if (delta < 86400)    return std::to_string(delta / 3600) + "h ago";
    if (delta < 86400*30) return std::to_string(delta / 86400) + "d ago";
    return format_date(unix_seconds);
}

}  // namespace

CardAction draw_account_panel(app::AppState& state, core::Account& a) {
    CardAction action = CardAction::None;

    ImGui::PushID(a.id.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 12.0F));

    ImGui::BeginChild("##panel-body", ImVec2(0, -kFooterReserved),
                      ImGuiChildFlags_NavFlattened);

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float text_h  = ImGui::GetTextLineHeight();
    const float sp_y    = ImGui::GetStyle().ItemSpacing.y;

    const bool any_refresh = a.web.last_refreshed_unix > 0
                          || a.bans.last_refreshed_unix > 0
                          || a.cs2.last_refreshed_unix > 0;

    BanPillsOptions opts;
    opts.show_vac       = state.settings.info.show_vac;
    opts.show_game      = state.settings.info.show_game_ban;
    opts.show_trade     = state.settings.info.show_trade_ban;
    opts.show_community = state.settings.info.show_community_ban;
    opts.show_vac_live  = state.settings.info.show_vac_live;
    opts.vac_live       = a.cs2.vac_live;
    const bool any_ban_pill = opts.show_vac || opts.show_game || opts.show_trade ||
                              opts.show_community || (opts.show_vac_live && opts.vac_live);

    constexpr float kBadgeH = 26.0F;
    constexpr float kWinsGap = 3.0F;
    constexpr float kModeGap = 2.0F;
    constexpr float kRankGap = 18.0F;
    constexpr float kRankLabelScale = 1.0F;

    const rank_image::TexEntry* premier_e = nullptr;
    if (state.settings.info.show_premier) {
        const int bracket = a.cs2.premier_rating > 0
            ? premier_bracket_for_rating(a.cs2.premier_rating) : 0;
        premier_e = rank_image::premier(bracket);
    }
    const rank_image::TexEntry* wingman_e = nullptr;
    if (state.settings.info.show_wingman) {
        const int rank = a.cs2.wingman_rank > 0 ? a.cs2.wingman_rank : 0;
        wingman_e = rank_image::wingman(rank);
    }
    const bool draw_premier = !a.is_nfa && premier_e && premier_e->srv && premier_e->h > 0;
    const bool draw_wingman = !a.is_nfa && wingman_e && wingman_e->srv && wingman_e->h > 0;
    const bool show_mode_label = draw_premier && draw_wingman;
    const float label_h      = text_h * kRankLabelScale;
    const float mode_block   = show_mode_label ? (label_h + kModeGap) : 0.0F;
    const float rank_block_h = mode_block + kBadgeH + kWinsGap + label_h;
    const float premier_w = draw_premier
        ? kBadgeH * (static_cast<float>(premier_e->w) / static_cast<float>(premier_e->h))
        : 0.0F;
    const float wingman_w = draw_wingman
        ? kBadgeH * (static_cast<float>(wingman_e->w) / static_cast<float>(wingman_e->h))
        : 0.0F;
    const float rank_total_w = premier_w
                              + (draw_premier && draw_wingman ? kRankGap : 0.0F)
                              + wingman_w;

    struct ChipSpec {
        std::string label;
        std::string value;
        ImVec4 color{};
        bool has_color = false;
        float width = 0.0F;
    };
    std::vector<ChipSpec> chips;
    auto push_chip = [&](const char* label, const char* value,
                          const ImVec4* col = nullptr) {
        ChipSpec s;
        s.label = label;
        s.value = value;
        if (col) { s.has_color = true; s.color = *col; }
        constexpr float pad_x = 8.0F;
        constexpr float inner_gap = 5.0F;
        s.width = ImGui::CalcTextSize(label).x + inner_gap +
                  ImGui::CalcTextSize(value).x + pad_x * 2.0F;
        chips.push_back(std::move(s));
    };
    {
        char buf[40];
        if (state.settings.info.show_steam_level && a.web.steam_level >= 0) {
            std::snprintf(buf, sizeof(buf), "%d", a.web.steam_level);
            push_chip("Steam Lv", buf);
        }
        if (state.settings.info.show_owned_games && a.web.owned_games_count >= 0) {
            if (a.web.total_playtime_minutes > 0) {
                std::snprintf(buf, sizeof(buf), "%d \xC2\xB7 %lldh",
                              a.web.owned_games_count,
                              static_cast<long long>(a.web.total_playtime_minutes / 60));
            } else {
                std::snprintf(buf, sizeof(buf), "%d", a.web.owned_games_count);
            }
            push_chip("Games", buf);
        }
        if (!a.is_nfa && state.settings.info.show_prime) {
            const ImVec4 col = a.cs2.prime_status
                ? theme::success() : theme::dim_text();
            push_chip("Prime", a.cs2.prime_status ? "yes" : "no", &col);
        }
        if (!a.is_nfa && a.cs2.cs2_player_level >= 0) {
            std::snprintf(buf, sizeof(buf), "%d", a.cs2.cs2_player_level);
            push_chip("CS2 Lv", buf);
        }
        if (!a.is_nfa && a.cs2.cs2_player_xp >= 0) {
            const std::string xp = format_with_commas(a.cs2.cs2_player_xp);
            push_chip("XP", xp.c_str());
        }
        if (state.settings.info.show_external_funds && a.funds.total_spend_usd_cents >= 0) {
            if (a.funds.currency_is_usd) {
                std::snprintf(buf, sizeof(buf), "$%s.%02d",
                              format_with_commas(static_cast<int>(a.funds.total_spend_usd_cents / 100)).c_str(),
                              static_cast<int>(a.funds.total_spend_usd_cents % 100));
                const ImVec4 col = a.funds.total_spend_usd_cents >= 35000 ? theme::success()
                                 : a.funds.total_spend_usd_cents >= 25000 ? theme::warning()
                                 : theme::danger();
                push_chip("Spent", buf, &col);
            } else {
                std::snprintf(buf, sizeof(buf), "%s %s",
                              format_with_commas(static_cast<int>(a.funds.total_spend_usd_cents / 100)).c_str(),
                              a.funds.currency.c_str());
                push_chip("Spent", buf);
            }
        }
        if (a.steam_id_64 != 0) {
            char sid[24];
            std::snprintf(sid, sizeof(sid), "%llu",
                          static_cast<unsigned long long>(a.steam_id_64));
            push_chip("Steam ID", sid);
        }
        if (a.is_nfa && a.refresh_token_expires > 0) {
            const std::int64_t remaining = a.refresh_token_expires - now_seconds();
            char tbuf[24];
            if (remaining <= 0) {
                std::snprintf(tbuf, sizeof(tbuf), "expired");
            } else if (remaining < 86400) {
                std::snprintf(tbuf, sizeof(tbuf), "%lldh",
                              static_cast<long long>(remaining / 3600));
            } else {
                std::snprintf(tbuf, sizeof(tbuf), "%lldd",
                              static_cast<long long>(remaining / 86400));
            }
            const ImVec4 col = remaining <= 0           ? theme::danger()
                             : remaining < 7 * 86400     ? theme::warning()
                                                         : theme::success();
            push_chip("NFA token", tbuf, &col);
        }
    }

    constexpr float kChipGap = 6.0F;
    std::vector<std::pair<std::size_t, std::size_t>> chip_rows;
    {
        std::size_t start = 0;
        float row_w = 0.0F;
        for (std::size_t i = 0; i < chips.size(); ++i) {
            const float add = (i == start) ? chips[i].width
                                            : kChipGap + chips[i].width;
            if (i == start || row_w + add <= avail_w) {
                row_w += add;
            } else {
                chip_rows.emplace_back(start, i);
                start = i;
                row_w = chips[i].width;
            }
        }
        if (start < chips.size()) chip_rows.emplace_back(start, chips.size());
    }

    // Report the OLDEST applicable freshness timestamp: the bulk Web API call
    // updates web/bans in seconds but the per-account GCPD scrape (a.cs2) is
    // staggered and can lag minutes, so folding cs2 in keeps "Xh ago" honest.
    // Skipped when GCPD isn't applicable so the line doesn't stick at "never".
    std::int64_t last_refresh = 0;
    {
        auto fold = [&](std::int64_t t) {
            if (t <= 0) return;
            last_refresh = (last_refresh == 0) ? t : std::min(last_refresh, t);
        };
        fold(a.web.last_refreshed_unix);
        fold(a.bans.last_refreshed_unix);
        const bool gcpd_applicable = state.settings.gcpd_enabled
                                      && !a.session_id.empty()
                                      && !a.steam_login_secure.empty();
        if (gcpd_applicable) fold(a.cs2.last_refreshed_unix);
    }
    std::string meta_line;
    if (a.web.created_unix > 0) {
        meta_line += "Created ";
        meta_line += format_date(a.web.created_unix);
    }
    if (a.web.created_unix > 0 || last_refresh > 0) {
        if (!meta_line.empty()) meta_line += " \xC2\xB7 ";
        meta_line += "Last refreshed ";
        meta_line += format_relative(last_refresh);
    }
    const bool has_meta_line = !meta_line.empty();

    const auto now_s = now_seconds();
    const bool has_cooldown = state.settings.info.show_cooldown
                              && a.cs2.cooldown_expires_unix > now_s;
    std::string cooldown_line;
    if (has_cooldown) {
        const char* reason = a.cs2.cooldown_reason.empty()
            ? "Cooldown active" : a.cs2.cooldown_reason.c_str();
        char buf[160];
        if (a.cs2.cooldown_expires_unix == sam::core::kCooldownNever) {
            std::snprintf(buf, sizeof(buf), "%s \xC2\xB7 Permanent", reason);
        } else {
            const auto remaining = a.cs2.cooldown_expires_unix - now_s;
            if (remaining >= 86400) {
                std::snprintf(buf, sizeof(buf), "%s \xC2\xB7 %lldd left",
                              reason, static_cast<long long>(remaining / 86400));
            } else if (remaining >= 3600) {
                std::snprintf(buf, sizeof(buf), "%s \xC2\xB7 %lldh left",
                              reason, static_cast<long long>(remaining / 3600));
            } else {
                std::snprintf(buf, sizeof(buf), "%s \xC2\xB7 %lldm left",
                              reason, static_cast<long long>(remaining / 60));
            }
        }
        cooldown_line = buf;
    }

    const bool has_weekly_drop = state.settings.info.show_weekly_drop
                                 && a.cs2.weekly_drop_reset_unix > now_s;
    std::string weekly_drop_line;
    if (has_weekly_drop) {
        const auto remaining = a.cs2.weekly_drop_reset_unix - now_s;
        char buf[96];
        if (remaining >= 86400) {
            std::snprintf(buf, sizeof(buf), "Weekly drop claimed \xC2\xB7 resets in %lldd",
                          static_cast<long long>(remaining / 86400));
        } else if (remaining >= 3600) {
            std::snprintf(buf, sizeof(buf), "Weekly drop claimed \xC2\xB7 resets in %lldh",
                          static_cast<long long>(remaining / 3600));
        } else {
            std::snprintf(buf, sizeof(buf), "Weekly drop claimed \xC2\xB7 resets in %lldm",
                          static_cast<long long>(remaining / 60));
        }
        weekly_drop_line = buf;
    }

    const float chip_h = text_h + 6.0F;
    float middle_h = 0.0F;
    if (draw_premier || draw_wingman) middle_h += rank_block_h;
    // Each section's leading ImGui::Spacing() is an empty item that adds two
    // item-spacings of gap, not one; use 2*sp_y so this estimate matches render.
    if (!chip_rows.empty()) {
        if (middle_h > 0) middle_h += 2.0F * sp_y;
        middle_h += static_cast<float>(chip_rows.size()) * chip_h;
        if (chip_rows.size() > 1) {
            middle_h += static_cast<float>(chip_rows.size() - 1) * sp_y;
        }
    }
    if (has_meta_line)      middle_h += 2.0F * sp_y + text_h;
    if (has_cooldown)       middle_h += 2.0F * sp_y + text_h;
    if (has_weekly_drop)    middle_h += 2.0F * sp_y + text_h;
    if (!a.tag_ids.empty()) middle_h += 2.0F * sp_y + chip_h;
    if (!state.settings.hide_notes && !a.notes.empty())   middle_h += 2.0F * sp_y + text_h * 2.0F;

    auto center_h_lead = [&](float section_w) {
        const float lead = (avail_w - section_w) * 0.5F;
        if (lead > 0) {
            ImGui::Dummy(ImVec2(lead, 0));
            ImGui::SameLine(0.0F, 0.0F);
        }
    };

    const ImVec2 header_origin_screen = ImGui::GetCursorScreenPos();
    const float  pane_inner_w         = ImGui::GetContentRegionAvail().x;

    // Identity lines read better tightly stacked; the 12px panel spacing is for
    // the stat sections lower down.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 3.0F));
    draw_avatar(a, kAvatarSize);
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::BeginGroup();
    {
        ImFont* tf = fonts::title();
        if (tf) ImGui::PushFont(tf, 0.0F);
        if (!a.web.persona_name.empty()) {
            ImGui::TextUnformatted(a.web.persona_name.c_str());
        } else {
            draw_login_text(state, a);
        }
        if (tf) ImGui::PopFont();
    }
    if (a.login_method != core::LoginMethod::Normal) {
        ImGui::SameLine();
        draw_login_method_chip(a.login_method);
    }
    if (!a.web.persona_name.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        draw_login_text(state, a);
        ImGui::PopStyleColor();
    }
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        bool any = false;
        auto sep = [&]() {
            if (any) { ImGui::SameLine(); ImGui::TextDisabled("\xC2\xB7"); ImGui::SameLine(); }
            any = true;
        };
        if (!a.web.country_code.empty()) {
            sep();
            std::string cc;
            cc.reserve(a.web.country_code.size());
            for (char c : a.web.country_code)
                cc.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            ImGui::TextDisabled("%s", cc.c_str());
        }
        if (a.last_login_unix > 0) {
            sep();
            ImGui::TextDisabled("last login %s", format_relative(a.last_login_unix).c_str());
        }
        if (!any && !any_refresh) {
            ImGui::TextDisabled("no profile data yet");
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar();

    {
        const ImVec2 after_header = ImGui::GetCursorScreenPos();
        const float trust_x = header_origin_screen.x + pane_inner_w - 16.0F;
        const float trust_y = header_origin_screen.y + 4.0F;
        if (a.is_nfa) draw_nfa_pill(trust_x - 6.0F, trust_y + 7.0F);
        ImGui::SetCursorScreenPos(ImVec2(trust_x, trust_y));
        if (draw_trust_badge(a.trust, true)) {
            state.vault_dirty = true;
            state.save_vault_if_dirty();
        }
        ImGui::SetCursorScreenPos(after_header);
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (any_ban_pill) draw_ban_pills(a.bans, opts);

    // Center the rank + stat block between the ban pills and the panel-body
    // bottom; if it's taller than the remaining space, skip the spacer and let
    // the child window scroll.
    {
        const float y_cursor  = ImGui::GetCursorPosY();
        const float child_h   = ImGui::GetWindowHeight();
        const float remaining = child_h - y_cursor;
        if (middle_h < remaining) {
            ImGui::Dummy(ImVec2(0, (remaining - middle_h) * 0.5F));
        }
    }

    if (draw_premier || draw_wingman) {
        auto* dl = ImGui::GetWindowDrawList();
        center_h_lead(rank_total_w);

        auto draw_card = [&](const rank_image::TexEntry* e, const char* mode,
                              int wins, int rating, bool rating_overlay,
                              float card_w) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(card_w, rank_block_h));

            const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            ImFont* lf = ImGui::GetFont();
            const float label_size = lf->LegacySize * kRankLabelScale;
            if (show_mode_label) {
                const ImVec2 ts = lf->CalcTextSizeA(label_size, FLT_MAX, 0.0F, mode);
                dl->AddText(lf, label_size,
                            ImVec2(origin.x + (card_w - ts.x) * 0.5F, origin.y),
                            dim, mode);
            }
            const float badge_y = origin.y + mode_block;
            dl->AddImage(reinterpret_cast<ImTextureID>(e->srv),
                         ImVec2(origin.x, badge_y),
                         ImVec2(origin.x + card_w, badge_y + kBadgeH));
            if (rating_overlay) {
                const std::string text = rating > 0
                    ? format_with_commas(rating) : std::string("---");
                ImFont* font = fonts::badge();
                const float fsize = font->LegacySize * (kBadgeH / 26.0F);
                const ImVec2 ts = font->CalcTextSizeA(fsize, FLT_MAX, 0.0F, text.c_str());
                const float tx = origin.x + card_w * 0.58F - ts.x * 0.5F;
                const float ty = badge_y + (kBadgeH - fsize) * 0.5F - 2.0F;
                const ImU32 col = rating > 0
                    ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 140);
                dl->AddText(font, fsize, ImVec2(tx, ty), col, text.c_str());
            }
            char buf[32];
            if (wins >= 0) std::snprintf(buf, sizeof(buf), "Wins: %d", wins);
            else           std::snprintf(buf, sizeof(buf), "Wins: ---");
            const ImVec2 ws = lf->CalcTextSizeA(label_size, FLT_MAX, 0.0F, buf);
            dl->AddText(lf, label_size,
                        ImVec2(origin.x + (card_w - ws.x) * 0.5F,
                                badge_y + kBadgeH + kWinsGap),
                        dim, buf);
        };

        if (draw_premier) {
            draw_card(premier_e, "PREMIER",
                      a.cs2.premier_wins, a.cs2.premier_rating, true, premier_w);
            if (draw_wingman) ImGui::SameLine(0.0F, kRankGap);
        }
        if (draw_wingman) {
            draw_card(wingman_e, "WINGMAN",
                      a.cs2.wingman_wins, 0, false, wingman_w);
        }
    }

    if (!chip_rows.empty()) {
        ImGui::Spacing();
        for (std::size_t r = 0; r < chip_rows.size(); ++r) {
            const auto [start, end] = chip_rows[r];
            float row_total = 0.0F;
            for (std::size_t i = start; i < end; ++i) row_total += chips[i].width;
            if (end > start + 1) {
                row_total += kChipGap * static_cast<float>(end - start - 1);
            }
            center_h_lead(row_total);
            for (std::size_t i = start; i < end; ++i) {
                if (i > start) ImGui::SameLine(0.0F, kChipGap);
                const auto& c = chips[i];
                if (c.has_color) draw_stat_chip(c.label.c_str(), c.value.c_str(), c.color);
                else             draw_stat_chip(c.label.c_str(), c.value.c_str());
            }
        }
    }

    if (has_meta_line) {
        ImGui::Spacing();
        const ImVec2 ts = ImGui::CalcTextSize(meta_line.c_str());
        center_h_lead(ts.x);
        ImGui::TextDisabled("%s", meta_line.c_str());
    }

    if (has_cooldown) {
        ImGui::Spacing();
        const ImVec2 ts = ImGui::CalcTextSize(cooldown_line.c_str());
        center_h_lead(ts.x);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextUnformatted(cooldown_line.c_str());
        ImGui::PopStyleColor();
    }

    if (has_weekly_drop) {
        ImGui::Spacing();
        const ImVec2 ts = ImGui::CalcTextSize(weekly_drop_line.c_str());
        center_h_lead(ts.x);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::TextUnformatted(weekly_drop_line.c_str());
        ImGui::PopStyleColor();
    }

    if (!a.tag_ids.empty()) {
        ImGui::Spacing();
        bool wrapped = false;
        for (const auto& tag_id : a.tag_ids) {
            for (const auto& t : state.vault.tags) {
                if (t.id == tag_id) {
                    if (wrapped) ImGui::SameLine();
                    draw_tag_chip(t.name, t.color_rgba);
                    wrapped = true;
                    break;
                }
            }
        }
        if (wrapped) ImGui::NewLine();
    }

    if (!state.settings.hide_notes && !a.notes.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(theme::dim_text(), "Notes");
        const std::string notes_utf8 = to_utf8(a.notes);
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(notes_utf8.c_str());
        ImGui::PopTextWrapPos();
    }

    if (state.settings.notifications.enabled) {
        auto unacked = state.notifications.unacked_for(a.id);
        if (!unacked.empty()) {
            ImGui::Spacing();
            char header[64];
            std::snprintf(header, sizeof(header), "Recent changes (%zu)###recent-changes",
                          unacked.size());
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::CollapsingHeader(header)) {
                for (const auto& ev : unacked) {
                    ImGui::PushID(ev.event_id.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ev.kind == core::BanEventKind::CooldownStarted ||
                        ev.kind == core::BanEventKind::CooldownEnded
                            ? theme::warning() : theme::danger());
                    ImGui::TextUnformatted(core::ban_event_kind_label(ev.kind));
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", format_relative(ev.detected_unix).c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Dismiss")) {
                        state.notifications.acknowledge(ev.event_id);
                    }
                    ImGui::PopID();
                }
                ImGui::Spacing();
                if (ImGui::SmallButton("Acknowledge all")) {
                    state.notifications.acknowledge_all_for(a.id);
                }
            }
        }
    }

    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0.0F, kFooterGap));

    const int n_buttons = a.sda.has_value() ? 5 : 4;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float content_w = ImGui::GetContentRegionAvail().x;
    // Reserve the Login button's attached caret so the rest stay equal-width.
    const float btn_w =
        (content_w - kLoginCaretWidth - spacing * static_cast<float>(n_buttons - 1)) /
        static_cast<float>(n_buttons);

    if (draw_login_split_button(state, a, btn_w + kLoginCaretWidth)) {
        action = CardAction::Launch;
    }
    ImGui::SameLine();
    if (a.sda.has_value()) {
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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (refresh_busy) {
                set_tooltip("Refresh in progress...");
            } else if (cooldown > 0) {
                set_tooltip("Wait %llds before refreshing again.",
                                  static_cast<long long>(cooldown));
            }
        }
    }
    ImGui::SameLine();
    if (action_button("Edit",   ImVec2(btn_w, 0))) action = CardAction::Edit;
    ImGui::SameLine();
    if (action_button("Remove", ImVec2(btn_w, 0))) action = CardAction::Remove;

    ImGui::PopStyleVar();
    ImGui::PopID();
    return action;
}

}  // namespace sam::ui::widgets
