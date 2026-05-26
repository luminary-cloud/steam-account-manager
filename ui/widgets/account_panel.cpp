#include "ui/widgets/account_panel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

#include <imgui.h>

#include "ui/fonts.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar.hpp"
#include "ui/widgets/ban_pills.hpp"
#include "ui/widgets/rank_image.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/status_markers.hpp"
#include "ui/widgets/tag_chip.hpp"
#include "ui/widgets/trust_badge.hpp"

#include "core/account_store/ban_diff.hpp"
#include "core/account_store/ban_event.hpp"

namespace sam::ui::widgets {

namespace {

constexpr float kAvatarSize = 56.0F;
constexpr float kFooterReserved = 44.0F;
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

void info_row(const char* label, const std::string& value) {
    if (value.empty()) return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(theme::dim_text(), "%s", label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

}  // namespace

CardAction draw_account_panel(app::AppState& state, core::Account& a) {
    CardAction action = CardAction::None;

    ImGui::PushID(a.id.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.0F));

    ImGui::BeginChild("##panel-body", ImVec2(0, -kFooterReserved),
                      ImGuiChildFlags_NavFlattened);

    const ImVec2 header_origin_screen = ImGui::GetCursorScreenPos();
    const float  pane_inner_w         = ImGui::GetContentRegionAvail().x;

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
    if (!a.web.persona_name.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        draw_login_text(state, a);
        ImGui::PopStyleColor();
    }

    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        bool any = false;
        auto sep = [&]() {
            if (any) { ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine(); }
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
        if (a.steam_id_64 != 0) {
            sep();
            ImGui::TextDisabled("%llu", static_cast<unsigned long long>(a.steam_id_64));
        }
        if (a.last_login_unix > 0) {
            sep();
            ImGui::TextDisabled("last login %s", format_relative(a.last_login_unix).c_str());
        }
        if (!any) {
            ImGui::TextDisabled("no profile data yet");
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    {
        const ImVec2 after_header = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(header_origin_screen.x + pane_inner_w - 16.0F,
                                          header_origin_screen.y + 4.0F));
        if (draw_trust_badge(a.trust, true)) {
            state.vault_dirty = true;
            state.save_vault_if_dirty();
        }
        ImGui::SetCursorScreenPos(after_header);
    }

    ImGui::Spacing();
    ImGui::Separator();

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

    {
        constexpr float kBadgeH = 32.0F;
        constexpr float kWinsGap = 3.0F;
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
            auto* dl = ImGui::GetWindowDrawList();
            const float wins_h = ImGui::GetTextLineHeight();

            auto wins_label = [&](const ImVec2& at, float bw, int wins) {
                char buf[32];
                if (wins >= 0) std::snprintf(buf, sizeof(buf), "Wins: %d", wins);
                else           std::snprintf(buf, sizeof(buf), "Wins: ---");
                const ImVec2 ts = ImGui::CalcTextSize(buf);
                const float tx = at.x + (bw - ts.x) * 0.5F;
                const float ty = at.y - kWinsGap - wins_h;
                dl->AddText(ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), buf);
            };

            ImGui::Dummy(ImVec2(0.0F, wins_h + kWinsGap));

            if (draw_premier) {
                const float w = kBadgeH * (static_cast<float>(premier_e->w) /
                                            static_cast<float>(premier_e->h));
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(w, kBadgeH));
                dl->AddImage(reinterpret_cast<ImTextureID>(premier_e->srv),
                             cursor, ImVec2(cursor.x + w, cursor.y + kBadgeH));
                wins_label(cursor, w, a.cs2.premier_wins);

                const std::string text = a.cs2.premier_rating > 0
                    ? format_with_commas(a.cs2.premier_rating)
                    : std::string("---");
                ImFont* font = fonts::badge();
                const float fsize = font->LegacySize;
                const ImVec2 ts = font->CalcTextSizeA(fsize, FLT_MAX, 0.0F, text.c_str());
                const float tx = cursor.x + w * 0.58F - ts.x * 0.5F;
                const float ty = cursor.y + (kBadgeH - fsize) * 0.5F - 2.0F;
                const ImU32 col = a.cs2.premier_rating > 0
                    ? IM_COL32_WHITE
                    : IM_COL32(255, 255, 255, 140);
                dl->AddText(font, fsize, ImVec2(tx, ty), col, text.c_str());

                if (draw_wingman) ImGui::SameLine(0.0F, 18.0F);
            }

            if (draw_wingman) {
                const float w = kBadgeH * (static_cast<float>(wingman_e->w) /
                                            static_cast<float>(wingman_e->h));
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(w, kBadgeH));
                dl->AddImage(reinterpret_cast<ImTextureID>(wingman_e->srv),
                             cursor, ImVec2(cursor.x + w, cursor.y + kBadgeH));
                wins_label(cursor, w, a.cs2.wingman_wins);
            }
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0F, 2.0F));
    if (ImGui::BeginTable("##panel-info", 2,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoBordersInBody,
            ImVec2(0, 0))) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 150.0F);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);

        if (state.settings.info.show_steam_level && a.web.steam_level >= 0) {
            info_row("Steam level", std::to_string(a.web.steam_level));
        }
        if (state.settings.info.show_owned_games && a.web.owned_games_count >= 0) {
            info_row("Games owned", std::to_string(a.web.owned_games_count));
            if (a.web.total_playtime_minutes > 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld h",
                              static_cast<long long>(a.web.total_playtime_minutes / 60));
                info_row("Total playtime", buf);
            }
        }
        if (state.settings.info.show_prime) {
            info_row("Prime", a.cs2.prime_status ? "yes" : "no");
        }
        if (a.cs2.cs2_player_level >= 0) {
            info_row("CS2 player level", std::to_string(a.cs2.cs2_player_level));
        }
        if (a.cs2.cs2_player_xp >= 0) {
            info_row("CS2 XP", format_with_commas(a.cs2.cs2_player_xp));
        }
        if (a.web.created_unix > 0) {
            info_row("Account created", format_date(a.web.created_unix));
        }
        // Report the OLDEST applicable freshness timestamp so the row tells the
        // truth during the launch refresh window: the bulk Web API call updates
        // web/bans within seconds, but the per-account GCPD scrape (which
        // produces a.cs2) is staggered and can lag by minutes for large vaults.
        // Including cs2 here pulls the display back to honest "Xh ago" until the
        // scrape lands. Skipped when GCPD isn't applicable (disabled, or no
        // session cookies) so the row doesn't get stuck at "never".
        std::int64_t last_refresh = 0;
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
        info_row("Last refreshed", format_relative(last_refresh));

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    if (state.settings.info.show_cooldown && a.cs2.cooldown_expires_unix > 0) {
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (a.cs2.cooldown_expires_unix > now_s) {
            const auto remaining = a.cs2.cooldown_expires_unix - now_s;
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            const char* reason = a.cs2.cooldown_reason.empty()
                ? "Cooldown active" : a.cs2.cooldown_reason.c_str();
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

    if (!a.notes.empty()) {
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
    const float btn_w = (content_w - spacing * static_cast<float>(n_buttons - 1)) /
                        static_cast<float>(n_buttons);

    if (action_button("Login", ImVec2(btn_w, 0))) action = CardAction::Launch;
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
                ImGui::SetTooltip("Refresh in progress...");
            } else if (cooldown > 0) {
                ImGui::SetTooltip("Wait %llds before refreshing again.",
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
