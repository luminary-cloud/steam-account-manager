#include "ui/screens/trade_offers_detail.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "app/job_pump.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/trade/actions.hpp"
#include "core/trade/inventory.hpp"
#include "core/trade/offers.hpp"
#include "core/trade/trade_audit.hpp"
#include "core/trade/trade_url.hpp"
#include "ui/screens/confirmations_screen.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/search_bar.hpp"
#include "ui/widgets/toast_stack.hpp"

namespace sam::ui::screens {

namespace {

namespace trade = core::trade;

using namespace trade_offers_detail;

// account_id empty -> no preselection; otherwise preselect just that account.
void open_send_modal_impl(app::AppState& state, const std::string& account_id) {
    g_send.accounts.clear();
    if (!account_id.empty()) g_send.accounts.insert(account_id);
    std::snprintf(g_send.url_buf, sizeof(g_send.url_buf), "%s",
                  state.settings.trade.default_destination_trade_url.c_str());
    g_send.msg_buf[0] = '\0';
    g_send.search_buf[0] = '\0';
    g_send.acct_search[0] = '\0';
    g_send.selected.clear();
    g_send.bulk_ack = false;
    g_send.inv_cache_unix = -1;
    g_send.open_request = true;
}

void bulk_send_all(app::AppState& state, std::vector<core::Account> accs,
                   trade::TradeUrl tu);

// Remember url in the saved-links list (deduped) and as the default destination.
void remember_trade_link(app::AppState& state, const std::string& url) {
    auto& saved = state.settings.trade.saved_trade_urls;
    if (std::none_of(saved.begin(), saved.end(),
                     [&](const auto& l) { return l.url == url; }))
        saved.push_back({url, {}});
    state.settings.trade.default_destination_trade_url = url;
    state.save_settings();
}

void draw_send_modal_impl(app::AppState& state) {
    if (g_send.open_request) {
        ImGui::OpenPopup("Send trade offer");
        g_send.open_request = false;
    }
    ImGui::SetNextWindowSize(ImVec2(720.0F, 680.0F), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    if (!ImGui::BeginPopupModal("Send trade offer", nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar(2);
        return;
    }

    // Inventory grid fixed at 3 rows; account grid flexes to fill the rest, with the
    // footer pinned to the bottom so the popup never scrolls.
    const float kLine  = ImGui::GetTextLineHeightWithSpacing();
    const float kFrame = ImGui::GetFrameHeightWithSpacing();
    constexpr float kChipH   = 44.0F;
    constexpr float kChipGap = 8.0F;
    const float items_h = 3.0F * (kTile + ImGui::GetStyle().ItemSpacing.y) + 10.0F;
    const float footer_pin_y = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y -
                               ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.y;
    const float middle_fixed =
        kLine            // account hint line
        + kLine          // link label
        + kFrame         // link combo/input row
        + kLine          // validity line
        + kFrame         // separator + spacing
        + kFrame         // items/bulk load+filter row
        + items_h        // items/bulk body (fixed 3 rows)
        + ImGui::GetStyle().ItemSpacing.y * 1.5F;  // inter-section spacing + margin

    ImGui::TextDisabled("Source accounts");
    ImGui::SameLine();
    ImGui::Text("(%zu selected)", g_send.accounts.size());
    ImGui::SameLine();
    if (action_button("Clear")) {
        g_send.accounts.clear();
        g_send.selected.clear();
        g_send.inv_cache_unix = -1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0F);
    ImGui::InputTextWithHint("##acctsearch", "filter", g_send.acct_search,
                             sizeof(g_send.acct_search));

    const std::string af = core::to_lower(g_send.acct_search);
    std::vector<const core::Account*> accts;
    for (auto& a : state.vault.accounts) {
        if (!account_can_trade(a)) continue;
        if (!af.empty() &&
            core::to_lower(account_label(state, a)).find(af) == std::string::npos &&
            core::to_lower(a.login).find(af) == std::string::npos) continue;
        accts.push_back(&a);
    }

    const float acc_h = std::max(3.0F * (kChipH + ImGui::GetStyle().ItemSpacing.y),
                                 footer_pin_y - ImGui::GetCursorPosY() - middle_fixed);
    ImGui::BeginChild("##acctgrid", ImVec2(0, acc_h), ImGuiChildFlags_Borders);
    {
        const float gw = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, static_cast<int>(gw / 220.0F + 0.5F));
        const float chip_w = (gw - static_cast<float>(cols - 1) * kChipGap) /
                             static_cast<float>(cols);
        const float row_pitch = kChipH + ImGui::GetStyle().ItemSpacing.y;
        const int total = static_cast<int>(accts.size());
        const int rows = (total + cols - 1) / cols;
        auto* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin(rows, row_pitch);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int c = 0; c < cols; ++c) {
                    const int idx = row * cols + c;
                    if (idx >= total) break;
                    const core::Account& a = *accts[idx];
                    if (c > 0) ImGui::SameLine(0.0F, kChipGap);
                    ImGui::PushID(a.id.c_str());
                    const bool sel = g_send.accounts.count(a.id) != 0;
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    const ImVec2 p1 = ImVec2(p0.x + chip_w, p0.y + kChipH);
                    if (ImGui::InvisibleButton("##chip", ImVec2(chip_w, kChipH))) {
                        if (sel) g_send.accounts.erase(a.id);
                        else     g_send.accounts.insert(a.id);
                        g_send.selected.clear();
                        g_send.inv_cache_unix = -1;
                    }
                    const bool hov = ImGui::IsItemHovered();
                    ImU32 bg;
                    if (sel) {
                        ImVec4 ac = theme::accent();
                        ac.w = 0.20F;
                        bg = ImGui::ColorConvertFloat4ToU32(ac);
                    } else {
                        bg = ImGui::ColorConvertFloat4ToU32(hov ? theme::panel_hover()
                                                                : theme::panel());
                    }
                    dl->AddRectFilled(p0, p1, bg, 6.0F);

                    const float pad = 6.0F;
                    const float av = kChipH - 2.0F * pad;
                    const ImVec2 av0 = ImVec2(p0.x + pad, p0.y + pad);
                    const ImVec2 av1 = ImVec2(av0.x + av, av0.y + av);
                    const float av_round = av * (8.0F / 48.0F);
                    dl->AddRectFilled(av0, av1, IM_COL32(40, 50, 60, 180), av_round);
                    if (auto* srv = widgets::avatar_for(a.web.avatar_url_full)) {
                        dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv), av0, av1,
                                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, av_round);
                    }

                    const float tx0 = av1.x + 8.0F;
                    const float tx1 = p1.x - pad;
                    const std::string label = account_label(state, a);
                    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                    const float region_w = tx1 - tx0;
                    const float text_x = tx0 + std::max(0.0F, (region_w - ts.x) * 0.5F);
                    const float text_y = p0.y + (kChipH - ts.y) * 0.5F;
                    dl->PushClipRect(ImVec2(tx0, p0.y), ImVec2(tx1, p1.y), true);
                    dl->AddText(ImVec2(text_x, text_y),
                                ImGui::ColorConvertFloat4ToU32(theme::text()), label.c_str());
                    dl->PopClipRect();

                    if (sel) {
                        dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::accent()),
                                    6.0F, 2.0F);
                    }
                    if (hov && ts.x > region_w) set_tooltip("%s", label.c_str());
                    ImGui::PopID();
                }
            }
        }
        clipper.End();
    }
    ImGui::EndChild();

    const bool bulk = g_send.accounts.size() >= 2;
    const std::string single =
        g_send.accounts.size() == 1 ? *g_send.accounts.begin() : std::string{};
    if (g_send.accounts.empty()) {
        ImGui::TextDisabled("Click accounts: one to pick items, or several for a bulk send.");
    } else if (bulk) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::Text("%zu accounts - bulk: ALL tradable items will be sent.",
                    g_send.accounts.size());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("1 account - pick specific items below.");
    }
    ImGui::Spacing();

    ImGui::TextDisabled("Destination trade link");
    auto& saved = state.settings.trade.saved_trade_urls;
    int link_to_delete = -1;
    if (!saved.empty()) {
        ImGui::SetNextItemWidth(150.0F);
        if (begin_styled_combo("##saved", "Saved links")) {
            for (int i = 0; i < static_cast<int>(saved.size()); ++i) {
                const auto& l = saved[i];
                ImGui::PushID(i);
                const char* label = l.name.empty() ? l.url.c_str() : l.name.c_str();
                if (ImGui::Selectable(label))
                    std::snprintf(g_send.url_buf, sizeof(g_send.url_buf), "%s", l.url.c_str());
                if (!l.name.empty() && ImGui::IsItemHovered())
                    set_tooltip("%s", l.url.c_str());
                if (ImGui::BeginPopupContextItem("##linkctx")) {
                    if (ImGui::MenuItem("Rename")) {
                        g_send.rename_url = l.url;
                        std::snprintf(g_send.rename_buf, sizeof(g_send.rename_buf), "%s",
                                      l.name.c_str());
                        g_send.rename_open_request = true;
                    }
                    if (ImGui::MenuItem("Delete")) link_to_delete = i;
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            end_styled_combo();
        }
        ImGui::SameLine();
    }
    if (link_to_delete >= 0 && link_to_delete < static_cast<int>(saved.size())) {
        saved.erase(saved.begin() + link_to_delete);
        state.save_settings();
    }

    // Rename sub-modal for the link chosen via the combo's right-click menu.
    if (g_send.rename_open_request) {
        ImGui::OpenPopup("Rename saved link");
        g_send.rename_open_request = false;
    }
    if (begin_styled_modal("Rename saved link", 360.0F)) {
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##link-name", "Leave blank to show the URL",
                                 g_send.rename_buf, sizeof(g_send.rename_buf));
        ImGui::Spacing();
        if (action_button("Save", ImVec2(100, 0))) {
            for (auto& l : saved)
                if (l.url == g_send.rename_url) { l.name = g_send.rename_buf; break; }
            state.save_settings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        end_styled_modal();
    }

    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##url",
        "https://steamcommunity.com/tradeoffer/new/?partner=...&token=...",
        g_send.url_buf, sizeof(g_send.url_buf));

    const trade::TradeUrl tu = trade::parse_trade_url(g_send.url_buf);
    if (tu.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::Text("Valid - partner %u", tu.partner_account_id);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (action_button("Save link")) {
            remember_trade_link(state, g_send.url_buf);
        }
    } else if (g_send.url_buf[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextUnformatted("Invalid trade link");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    bool can_send = false;

    if (!single.empty()) {
        core::Account* acc = state.find_account(single);

        bool inv_loading = false;
        std::string inv_error;
        std::int64_t inv_loaded = 0;
        {
            std::lock_guard lk(g_mtx);
            auto& st = g_states[single];
            inv_loading = st.inv_loading;
            inv_error = st.inv_error;
            inv_loaded = st.inv_loaded_unix;
            // Rebuild the picker cache only when the inventory actually changes.
            if (inv_loaded != g_send.inv_cache_unix || g_send.inv_cache_aid != single) {
                g_send.inv_cache.clear();
                g_send.inv_cache.reserve(st.inventory.size());
                for (const auto& it : st.inventory) {
                    if (!it.tradable) continue;
                    g_send.inv_cache.push_back({it, core::to_lower(it.market_hash_name)});
                }
                g_send.inv_cache_unix = inv_loaded;
                g_send.inv_cache_aid = single;
            }
        }

        const auto inv_cd = cooldown_remaining(
            inv_loaded, state.settings.trade.inventory_cooldown_seconds);
        ImGui::BeginDisabled(inv_loading || inv_cd > 0);
        if (action_button(inv_loading ? "Loading..." : "Load CS2 inventory") && acc)
            submit_inventory_load(state, *acc);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && inv_cd > 0)
            set_tooltip("Wait %llds before reloading.", static_cast<long long>(inv_cd));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0F);
        ImGui::InputTextWithHint("##itemsearch", "Filter items",
                                 g_send.search_buf, sizeof(g_send.search_buf));
        ImGui::SameLine();
        ImGui::TextDisabled("%zu selected", g_send.selected.size());
        if (!inv_error.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::Text("(%s)", inv_error.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::BeginChild("##invgrid", ImVec2(0, items_h), ImGuiChildFlags_Borders);
        const std::string filt = core::to_lower(g_send.search_buf);
        std::vector<int> filtered;
        filtered.reserve(g_send.inv_cache.size());
        for (int i = 0; i < static_cast<int>(g_send.inv_cache.size()); ++i) {
            if (filt.empty() || g_send.inv_cache[i].lname.find(filt) != std::string::npos)
                filtered.push_back(i);
        }
        const float avail = ImGui::GetContentRegionAvail().x;
        constexpr float base_gap = 8.0F;
        const int cols =
            std::max(1, static_cast<int>((avail + base_gap) / (kTile + base_gap)));
        // Spread leftover width into the gaps so square tiles fill the row edge-to-edge.
        const float tile_gap =
            cols > 1 ? std::max(base_gap, (avail - static_cast<float>(cols) * kTile) /
                                              static_cast<float>(cols - 1))
                     : base_gap;
        const int total = static_cast<int>(filtered.size());
        if (total == 0) {
            ImGui::TextDisabled(g_send.inv_cache.empty()
                                    ? "Load your inventory to pick items."
                                    : "No tradable items match.");
        } else {
            auto* dl = ImGui::GetWindowDrawList();
            const int rows = (total + cols - 1) / cols;
            const float row_h = kTile + ImGui::GetStyle().ItemSpacing.y;
            ImGuiListClipper clipper;
            clipper.Begin(rows, row_h);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    for (int c = 0; c < cols; ++c) {
                        const int fi = row * cols + c;
                        if (fi >= total) break;
                        const auto& it = g_send.inv_cache[filtered[fi]].item;
                        if (c > 0) ImGui::SameLine(0.0F, tile_gap);
                        ImGui::PushID(fi);
                        const bool sel = g_send.selected.count(it.asset_id) != 0;
                        const ImVec2 c0 = ImGui::GetCursorScreenPos();
                        if (ImGui::InvisibleButton("##it", ImVec2(kTile, kTile))) {
                            if (sel) g_send.selected.erase(it.asset_id);
                            else     g_send.selected.insert(it.asset_id);
                        }
                        dl->AddRectFilled(c0, ImVec2(c0.x + kTile, c0.y + kTile),
                                          ImGui::ColorConvertFloat4ToU32(theme::panel_hover()), 6.0F);
                        if (auto* srv = widgets::avatar_for(economy_image_url(it.icon_url))) {
                            dl->AddImageRounded(
                                reinterpret_cast<ImTextureID>(srv),
                                ImVec2(c0.x + kIconInset, c0.y + kIconInset),
                                ImVec2(c0.x + kTile - kIconInset, c0.y + kTile - kIconInset),
                                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.0F);
                        }
                        if (sel) {
                            dl->AddRect(c0, ImVec2(c0.x + kTile, c0.y + kTile),
                                        ImGui::ColorConvertFloat4ToU32(theme::accent()), 6.0F, 2.5F);
                        }
                        if (ImGui::IsItemHovered() && !it.market_hash_name.empty())
                            set_tooltip("%s", it.market_hash_name.c_str());
                        ImGui::PopID();
                    }
                }
            }
            clipper.End();
        }
        ImGui::EndChild();

        ImGui::SetCursorPosY(footer_pin_y);
        ImGui::SetNextItemWidth(-220.0F);
        ImGui::InputTextWithHint("##msg", "Optional message",
                                 g_send.msg_buf, sizeof(g_send.msg_buf));
        ImGui::SameLine();
        can_send = tu.ok && !g_send.selected.empty();
        ImGui::BeginDisabled(!can_send);
        if (action_button("Send", ImVec2(100.0F, 0)) && acc) {
            std::vector<trade::TradeAssetRef> give;
            give.reserve(g_send.selected.size());
            for (const auto asset_id : g_send.selected) {
                trade::TradeAssetRef r;
                r.asset_id = asset_id;
                give.push_back(r);
            }
            remember_trade_link(state, g_send.url_buf);
            submit_send(state, *acc, tu, std::move(give), g_send.msg_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::BeginChild("##bulkbody", ImVec2(0, items_h), ImGuiChildFlags_Borders);
        if (bulk) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped(
                "Bulk send loads each inventory and sends ALL tradable CS2 items "
                "from the %zu selected accounts to the link above. This cannot be "
                "undone.", g_send.accounts.size());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::Checkbox("I understand", &g_send.bulk_ack);
        } else {
            ImGui::TextDisabled("Select at least one account above.");
        }
        ImGui::EndChild();

        ImGui::SetCursorPosY(footer_pin_y);
        can_send = bulk && tu.ok && g_send.bulk_ack;
        ImGui::BeginDisabled(!can_send);
        if (action_button(bulk ? "Send to all" : "Send", ImVec2(120.0F, 0))) {
            std::vector<core::Account> accs;
            for (auto& a : state.vault.accounts) {
                if (account_can_trade(a) && g_send.accounts.count(a.id)) accs.push_back(a);
            }
            remember_trade_link(state, g_send.url_buf);
            bulk_send_all(state, std::move(accs), tu);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (action_button("Cancel", ImVec2(100.0F, 0))) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

// Runs fn(creds); on a session error, does one cooldown-respecting auto-relogin and retries.
// Works for any result type with ok/needs_relogin members.
template <typename Fn>
auto with_relogin(app::AppState& state, core::Account& creds, Fn&& fn) {
    auto res = fn(creds);
    if (!res.ok && res.needs_relogin &&
        state.relogin_cooldown_seconds(creds.id) <= 0) {
        std::string e;
        if (state.auto_relogin(creds.id, creds, &e)) {
            apply_refreshed_tokens(state, creds);
            res = fn(creds);
        }
    }
    return res;
}

void bulk_accept_all(app::AppState& state) {
    struct Target {
        core::Account acc;
        std::string oid;
        std::uint64_t partner;
    };
    std::vector<Target> targets;
    {
        std::lock_guard lk(g_mtx);
        for (auto& a : state.vault.accounts) {
            if (!account_can_trade(a)) continue;
            const auto it = g_states.find(a.id);
            if (it == g_states.end()) continue;
            for (const auto& o : it->second.received) {
                if (o.state == trade::TradeOfferState::Active)
                    targets.push_back({a, o.offer_id, o.partner_steam_id_64});
            }
        }
    }
    if (targets.empty()) return;

    const int total = static_cast<int>(targets.size());
    push_toast_at(state, "bulk-accept",
                  "Bulk accept: starting for " + std::to_string(total) + " offer(s)...",
                  "", false, 0);

    const int stagger = std::max(1500, state.settings.trade.refresh_stagger_ms);
    app::job_pump::submit([&state, targets = std::move(targets), stagger, total]() mutable {
        int ok = 0;
        int failed = 0;
        int processed = 0;
        for (auto& t : targets) {
            core::Account creds = t.acc;
            auto res = with_relogin(state, creds, [&](core::Account& a) {
                return trade::accept_trade_offer(a, t.oid, t.partner);
            });
            apply_refreshed_tokens(state, creds);
            if (res.ok) {
                ++ok;
                state.post_ui_callback([&state, aid = t.acc.id, oid = t.oid] {
                    erase_offer(state, aid, oid, /*from_received=*/true);
                });
            } else {
                ++failed;
            }
            ++processed;
            state.post_ui_callback([&state, processed, total, ok] {
                push_toast_at(state, "bulk-accept",
                              "Bulk accept: " + std::to_string(processed) + "/" +
                                  std::to_string(total) + " done (" +
                                  std::to_string(ok) + " accepted)",
                              "", false, 0);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(stagger));
        }
        state.post_ui_callback([&state, ok, failed, total] {
            std::string msg = "Bulk accept done: " + std::to_string(ok) + " accepted";
            if (failed) msg += ", " + std::to_string(failed) + " failed";
            msg += " (" + std::to_string(total) + " offers)";
            const std::int64_t dwell =
                std::max(state.settings.notifications.toast_duration_seconds, 12);
            push_toast_at(state, "bulk-accept", msg, "", ok != total, now_unix() + dwell);
        });
    });
}

void bulk_send_all(app::AppState& state, std::vector<core::Account> accs,
                   trade::TradeUrl tu) {
    if (!tu.ok || accs.empty()) return;

    const int n = static_cast<int>(accs.size());
    push_toast_at(state, "bulk-send",
                  "Bulk send: starting for " + std::to_string(n) + " account(s)...",
                  "", false, 0);

    const int stagger = std::max(2000, state.settings.trade.refresh_stagger_ms);
    const bool auto_conf = state.settings.trade.auto_confirm_sent;
    app::job_pump::submit([&state, accs = std::move(accs), tu, stagger, auto_conf, n]() mutable {
        int sent_ok = 0;
        int failed = 0;
        int skipped = 0;
        int processed = 0;
        std::vector<trade::TradeAuditEntry> audit;
        auto post_progress = [&state, n](int done, int sent) {
            state.post_ui_callback([&state, done, n, sent] {
                push_toast_at(state, "bulk-send",
                              "Bulk send: " + std::to_string(done) + "/" +
                                  std::to_string(n) + " done (" +
                                  std::to_string(sent) + " sent)",
                              "", false, 0);
            });
        };
        for (auto& seed : accs) {
            core::Account creds = seed;
            auto inv = with_relogin(state, creds, [&](core::Account& a) {
                return trade::fetch_inventory(a, a.steam_id_64, 730, 2, 2000, 0);
            });
            if (tokens_changed(seed, creds)) apply_refreshed_tokens(state, creds);
            if (!inv.ok) {
                trade::TradeAuditEntry e;
                e.unix_time = now_unix();
                e.account_id = seed.id;
                e.account_login = seed.login;
                e.source = trade::TradeAuditSource::UserBulk;
                e.outcome = trade::TradeAuditOutcome::Failed;
                e.detail = "inventory fetch failed: " + inv.error;
                audit.push_back(std::move(e));
                ++failed;
                ++processed;
                post_progress(processed, sent_ok);
                continue;
            }

            std::vector<trade::TradeAssetRef> give;
            for (const auto& it : inv.items) {
                if (!it.tradable) continue;
                trade::TradeAssetRef r;
                r.app_id = 730;
                r.context_id = 2;
                r.asset_id = it.asset_id;
                r.amount = 1;
                give.push_back(r);
            }
            if (give.empty()) {
                ++skipped;
                ++processed;
                post_progress(processed, sent_ok);
                continue;
            }

            auto res = with_relogin(state, creds, [&](core::Account& a) {
                return trade::send_trade_offer(a, tu, give, "");
            });
            apply_refreshed_tokens(state, creds);

            trade::TradeAuditEntry e;
            e.unix_time = now_unix();
            e.account_id = seed.id;
            e.account_login = seed.login;
            e.item_count = static_cast<int>(give.size());
            e.source = trade::TradeAuditSource::UserBulk;
            if (res.ok) {
                ++sent_ok;
                bool confirmed = false;
                if (res.needs_confirmation && auto_conf) {
                    for (int att = 0; att < 3; ++att) {
                        if (att > 0)
                            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                        const auto cr = trade::confirm_sent_offer(creds, res.offer_id);
                        if (cr.ok) { confirmed = true; break; }
                        if (!cr.not_found) break;
                    }
                }
                e.offer_id = res.offer_id;
                e.outcome = !res.needs_confirmation ? trade::TradeAuditOutcome::Sent
                            : confirmed             ? trade::TradeAuditOutcome::SentAndConfirmed
                                                    : trade::TradeAuditOutcome::NeedsConfirmation;
            } else {
                ++failed;
                e.outcome = trade::TradeAuditOutcome::Failed;
                e.detail = res.error;
            }
            SAM_LOG_INFO("trade bulk send {}: offer={} items={} outcome={}", seed.login,
                         e.offer_id, e.item_count, static_cast<int>(e.outcome));
            audit.push_back(std::move(e));
            ++processed;
            post_progress(processed, sent_ok);
            std::this_thread::sleep_for(std::chrono::milliseconds(stagger));
        }
        state.post_ui_callback([&state, sent_ok, failed, skipped, n, audit] {
            std::string msg = "Bulk send done: " + std::to_string(sent_ok) + " sent";
            if (failed) msg += ", " + std::to_string(failed) + " failed";
            if (skipped) msg += ", " + std::to_string(skipped) + " skipped";
            msg += " (" + std::to_string(n) + " accounts)";
            const std::int64_t dwell =
                std::max(state.settings.notifications.toast_duration_seconds, 12);
            push_toast_at(state, "bulk-send", msg, "", sent_ok != n, now_unix() + dwell);
            for (const auto& e : audit) state.trade_audit.record(e);
            confirmations_trigger_refresh_all(state);
        });
    });
}

void draw_bulk_confirms_impl(app::AppState& state) {
    if (g_confirm_accept_open) {
        ImGui::OpenPopup("Accept all incoming?");
        g_confirm_accept_open = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    if (ImGui::BeginPopupModal("Accept all incoming?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Accept %d incoming offer(s) across all accounts?",
                    g_bulk_accept_count);
        ImGui::TextDisabled("Accepts run staggered to respect Steam rate limits.");
        ImGui::Spacing();
        if (action_button("Accept all", ImVec2(120, 0))) {
            bulk_accept_all(state);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void draw_history_modal_impl(app::AppState& state, bool* p_open) {
    if (!*p_open) return;
    ImGui::OpenPopup("Trade history");
    ImGui::SetNextWindowSize(ImVec2(760.0F, 480.0F), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    if (!ImGui::BeginPopupModal("Trade history", p_open,
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar(2);
        return;
    }

    static std::string g_history_search;
    widgets::draw_search_bar(g_history_search, 320.0F);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu entries)", state.trade_audit.entries().size());

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035F, 0.035F, 0.035F, 1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F);
    ImGui::BeginChild("##history-body", ImVec2(0, -36.0F));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0F, 1.0F, 1.0F, 0.025F));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##history", 5, flags)) {
        ImGui::TableSetupColumn("##time", ImGuiTableColumnFlags_WidthFixed, 110.0F);
        ImGui::TableSetupColumn("##account", ImGuiTableColumnFlags_WidthFixed, 140.0F);
        ImGui::TableSetupColumn("##result", ImGuiTableColumnFlags_WidthFixed, 120.0F);
        ImGui::TableSetupColumn("##source", ImGuiTableColumnFlags_WidthFixed, 60.0F);
        ImGui::TableSetupColumn("##detail", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Time");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Account");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Result");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Source");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Detail");
        ImGui::PopStyleColor();

        const std::string search_lower = core::to_lower(g_history_search);
        const auto& es = state.trade_audit.entries();
        for (auto it = es.rbegin(); it != es.rend(); ++it) {
            const auto& e = *it;
            if (!search_lower.empty() &&
                core::to_lower(e.account_login).find(search_lower) == std::string::npos &&
                core::to_lower(e.detail).find(search_lower) == std::string::npos) {
                continue;
            }
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            char tbuf[24];
            std::time_t t = static_cast<std::time_t>(e.unix_time);
            std::tm tm{};
            localtime_s(&tm, &t);
            std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
            ImGui::TextUnformatted(tbuf);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.account_login.c_str());

            ImGui::TableNextColumn();
            ImVec4 col = theme::success();
            const char* label = "Sent";
            switch (e.outcome) {
                case trade::TradeAuditOutcome::Sent: break;
                case trade::TradeAuditOutcome::SentAndConfirmed:
                    label = "Sent + confirmed";
                    break;
                case trade::TradeAuditOutcome::NeedsConfirmation:
                    col = theme::warning();
                    label = "Needs confirm";
                    break;
                case trade::TradeAuditOutcome::Failed:
                    col = theme::danger();
                    label = "Failed";
                    break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            ImGui::TextDisabled(
                "%s", e.source == trade::TradeAuditSource::UserBulk ? "bulk" : "user");

            ImGui::TableNextColumn();
            std::string detail =
                std::to_string(e.item_count) + (e.item_count == 1 ? " item" : " items");
            if (!e.offer_id.empty()) detail += "  offer " + e.offer_id;
            if (!e.detail.empty()) detail += "  " + e.detail;
            ImGui::TextWrapped("%s", detail.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor();
    ImGui::EndChild();

    if (action_button("Close", ImVec2(100, 0))) {
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

}  // namespace

namespace trade_offers_detail {

void open_send_modal(app::AppState& state, const std::string& account_id) {
    open_send_modal_impl(state, account_id);
}

void draw_send_modal(app::AppState& state) {
    draw_send_modal_impl(state);
}

void draw_bulk_confirms(app::AppState& state) {
    draw_bulk_confirms_impl(state);
}

void draw_history_modal(app::AppState& state, bool* p_open) {
    draw_history_modal_impl(state, p_open);
}

}  // namespace trade_offers_detail

}  // namespace sam::ui::screens
