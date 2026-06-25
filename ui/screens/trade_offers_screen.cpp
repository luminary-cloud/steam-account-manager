#include "ui/screens/trade_offers_screen.hpp"

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
#include "ui/screens/trade_offers_detail.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/search_bar.hpp"
#include "ui/widgets/toast_stack.hpp"

namespace sam::ui::screens {

namespace trade_offers_detail {

namespace {
namespace trade = core::trade;
}  // namespace

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string account_label(app::AppState& state, const core::Account& a) {
    return a.web.persona_name.empty() ? widgets::login_label(state, a)
                                      : a.web.persona_name;
}

// NFA token-injection accounts get AccessDenied on community endpoints, so they can't trade.
bool account_can_trade(const core::Account& a) {
    return !a.is_nfa && a.steam_id_64 != 0 && !a.refresh_token.empty();
}

std::string economy_image_url(const std::string& icon_url) {
    if (icon_url.empty()) return {};
    return "https://community.cloudflare.steamstatic.com/economy/image/" +
           icon_url + "/62fx62f";
}

std::string format_date(std::int64_t unix_s) {
    const auto t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void apply_refreshed_tokens(app::AppState& state, const core::Account& creds) {
    state.post_ui_callback([&state, aid = creds.id,
                            rt  = creds.refresh_token,
                            at  = creds.access_token,
                            exp = creds.access_token_expires,
                            ls  = creds.steam_login_secure,
                            sid = creds.session_id] {
        auto* a = state.find_account(aid);
        if (!a) return;
        a->refresh_token        = rt;
        a->access_token         = at;
        a->access_token_expires = exp;
        a->steam_login_secure   = ls;
        a->session_id           = sid;
        state.vault_dirty = true;
        state.save_vault_if_dirty();
    });
}

bool tokens_changed(const core::Account& before, const core::Account& after) {
    return before.access_token != after.access_token ||
           before.refresh_token != after.refresh_token ||
           before.steam_login_secure != after.steam_login_secure ||
           before.session_id != after.session_id;
}

// expires_at == 0 keeps the toast sticky; otherwise it is an absolute unix expiry.
void push_toast_at(app::AppState& state, const std::string& id, const std::string& msg,
                   const std::string& account_id, bool warning, std::int64_t expires_at) {
    ui::widgets::ToastItem t;
    t.id = id;
    t.message = msg;
    t.account_id = account_id;
    t.is_warning = warning;
    t.expires_at_unix = expires_at;
    state.toasts.push(std::move(t));
}

void push_toast(app::AppState& state, const std::string& id, const std::string& msg,
                const std::string& account_id, bool warning) {
    push_toast_at(state, id, msg, account_id, warning,
                  now_unix() + state.settings.notifications.toast_duration_seconds);
}

void recount_incoming(app::AppState& state) {
    int total = 0;
    {
        std::lock_guard lk(g_mtx);
        for (const auto& [aid, st] : g_states) {
            for (const auto& o : st.received) {
                if (o.state == trade::TradeOfferState::Active) ++total;
            }
        }
    }
    state.pending_trade_offers_count.store(total, std::memory_order_relaxed);
}

void erase_offer(app::AppState& state, const std::string& account_id,
                 const std::string& offer_id, bool from_received) {
    {
        std::lock_guard lk(g_mtx);
        auto& st = g_states[account_id];
        auto& vec = from_received ? st.received : st.sent;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [&](const trade::TradeOffer& o) {
                                     return o.offer_id == offer_id;
                                 }),
                  vec.end());
    }
    recount_incoming(state);
}

std::int64_t cooldown_remaining(std::int64_t last, int window) {
    if (window <= 0 || last == 0) return 0;
    const auto since = now_unix() - last;
    return since < window ? window - since : 0;
}

std::int64_t refresh_cooldown_remaining(const app::AppState& state, const std::string& aid) {
    std::int64_t last = 0;
    {
        std::lock_guard lk(g_mtx);
        if (auto it = g_states.find(aid); it != g_states.end())
            last = it->second.last_refresh_unix;
    }
    return cooldown_remaining(last, state.settings.trade.per_account_cooldown_seconds);
}

void submit_account_refresh(app::AppState& state, const core::Account& seed) {
    if (!account_can_trade(seed)) return;
    if (refresh_cooldown_remaining(state, seed.id) > 0) return;
    {
        std::lock_guard lk(g_mtx);
        auto& st = g_states[seed.id];
        if (st.refreshing) return;
        st.refreshing = true;
        st.last_error.clear();
    }

    const auto snapshot = seed;
    app::job_pump::submit([&state, snapshot]() mutable {
        core::Account creds = snapshot;
        auto result = trade::get_trade_offers(creds, /*active_only=*/true);

        if (!result.ok && result.needs_relogin) {
            const auto cd = state.relogin_cooldown_seconds(creds.id);
            if (cd > 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "login rate-limited, try again in %lld s",
                              static_cast<long long>(cd));
                result.error = buf;
            } else {
                std::string relogin_err;
                if (state.auto_relogin(creds.id, creds, &relogin_err)) {
                    apply_refreshed_tokens(state, creds);
                    result = trade::get_trade_offers(creds, true);
                } else {
                    result.error = "auto-relogin failed: " + relogin_err;
                }
            }
        }

        if (tokens_changed(snapshot, creds)) apply_refreshed_tokens(state, creds);

        const auto done = now_unix();
        {
            std::lock_guard lk(g_mtx);
            auto& st = g_states[creds.id];
            st.refreshing = false;
            st.last_refresh_unix = done;
            if (result.ok) {
                st.received = std::move(result.received);
                st.sent = std::move(result.sent);
                st.last_error.clear();
            } else {
                st.last_error = result.error;
            }
        }
        recount_incoming(state);
    });
}

void submit_inventory_load(app::AppState& state, const core::Account& seed) {
    if (!account_can_trade(seed)) return;
    {
        std::lock_guard lk(g_mtx);
        auto& st = g_states[seed.id];
        if (st.inv_loading) return;
        st.inv_loading = true;
        st.inv_error.clear();
    }

    const auto snapshot = seed;
    app::job_pump::submit([&state, snapshot]() mutable {
        core::Account creds = snapshot;
        auto res = trade::fetch_inventory(creds, creds.steam_id_64, 730, 2, 2000, 0);

        if (!res.ok && res.needs_relogin) {
            const auto cd = state.relogin_cooldown_seconds(creds.id);
            if (cd <= 0) {
                std::string e;
                if (state.auto_relogin(creds.id, creds, &e)) {
                    apply_refreshed_tokens(state, creds);
                    res = trade::fetch_inventory(creds, creds.steam_id_64, 730, 2, 2000, 0);
                } else {
                    res.error = "auto-relogin failed: " + e;
                }
            }
        }
        if (tokens_changed(snapshot, creds)) apply_refreshed_tokens(state, creds);

        {
            std::lock_guard lk(g_mtx);
            auto& st = g_states[creds.id];
            st.inv_loading = false;
            if (res.ok) {
                st.inventory = std::move(res.items);
                st.inv_error.clear();
                st.inv_loaded_unix = now_unix();
            } else {
                st.inv_error = res.error;
            }
        }
    });
}

void submit_send(app::AppState& state, const core::Account& acc, trade::TradeUrl tu,
                 std::vector<trade::TradeAssetRef> give, std::string message) {
    const std::string aid = acc.id;
    const std::string login = acc.login;
    const int item_count = static_cast<int>(give.size());
    const bool auto_conf = state.settings.trade.auto_confirm_sent;
    auto cap = acc;
    app::job_pump::submit([&state, cap, aid, login, item_count, tu, give, message,
                           auto_conf]() mutable {
        auto res = trade::send_trade_offer(cap, tu, give, message);

        if (!res.ok && res.needs_relogin) {
            const auto cd = state.relogin_cooldown_seconds(aid);
            if (cd <= 0) {
                std::string e;
                if (state.auto_relogin(aid, cap, &e)) {
                    apply_refreshed_tokens(state, cap);
                    res = trade::send_trade_offer(cap, tu, give, message);
                } else {
                    res.error = "auto-relogin failed: " + e;
                }
            }
        }
        apply_refreshed_tokens(state, cap);

        if (!res.ok) {
            const std::string err = res.error;
            SAM_LOG_INFO("trade send {}: failed items={} err={}", login, item_count, err);
            state.post_ui_callback([&state, aid, login, item_count, err] {
                push_toast(state, "send-fail-" + aid, "Send failed: " + err, aid, true);
                trade::TradeAuditEntry e;
                e.unix_time = now_unix();
                e.account_id = aid;
                e.account_login = login;
                e.item_count = item_count;
                e.outcome = trade::TradeAuditOutcome::Failed;
                e.detail = err;
                e.source = trade::TradeAuditSource::UserSingle;
                state.trade_audit.record(std::move(e));
            });
            return;
        }

        const std::string oid = res.offer_id;
        const bool needs_conf = res.needs_confirmation;
        bool confirmed = false;
        std::string conf_note;
        if (needs_conf && auto_conf) {
            for (int attempt = 0; attempt < 3 && !confirmed; ++attempt) {
                if (attempt > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                const auto cr = trade::confirm_sent_offer(cap, oid);
                if (cr.ok) { confirmed = true; break; }
                if (!cr.not_found) { conf_note = cr.error; break; }
            }
            if (!confirmed && conf_note.empty()) conf_note = "confirmation not found yet";
        }

        const trade::TradeAuditOutcome outcome =
            !needs_conf ? trade::TradeAuditOutcome::Sent
            : confirmed ? trade::TradeAuditOutcome::SentAndConfirmed
                        : trade::TradeAuditOutcome::NeedsConfirmation;
        SAM_LOG_INFO("trade send {}: offer={} items={} outcome={}", login, oid, item_count,
                     static_cast<int>(outcome));

        state.post_ui_callback([&state, aid, login, item_count, oid, needs_conf, auto_conf,
                                confirmed, conf_note, outcome] {
            if (!needs_conf) {
                push_toast(state, "send-" + oid, "Offer sent", aid, false);
            } else if (auto_conf && confirmed) {
                push_toast(state, "send-" + oid, "Offer sent and confirmed", aid, false);
            } else {
                std::string m = "Offer sent - confirm it in Confirmations";
                if (auto_conf && !conf_note.empty()) m += " (" + conf_note + ")";
                push_toast(state, "send-" + oid, m, aid, true);
                confirmations_trigger_refresh_all(state);
            }
            trade::TradeAuditEntry e;
            e.unix_time = now_unix();
            e.account_id = aid;
            e.account_login = login;
            e.offer_id = oid;
            e.item_count = item_count;
            e.outcome = outcome;
            if (outcome == trade::TradeAuditOutcome::NeedsConfirmation) e.detail = conf_note;
            e.source = trade::TradeAuditSource::UserSingle;
            state.trade_audit.record(std::move(e));
            // Clear cooldown so the next refresh shows the new outgoing offer immediately.
            {
                std::lock_guard lk(g_mtx);
                g_states[aid].last_refresh_unix = 0;
            }
            if (auto* a = state.find_account(aid)) submit_account_refresh(state, *a);
        });
    });
}

}  // namespace trade_offers_detail

namespace {

namespace trade = core::trade;

using namespace trade_offers_detail;

// kind: 'a' accept, 'd' decline, 'c' cancel.
void submit_action(app::AppState& state, const core::Account& acc,
                   const trade::TradeOffer& offer, char kind) {
    const std::string aid = acc.id;
    const std::string oid = offer.offer_id;
    const bool from_received = !offer.is_our_offer;
    const std::uint64_t partner = offer.partner_steam_id_64;
    {
        std::lock_guard lk(g_mtx);
        g_in_flight.insert(oid);
    }

    auto cap_acc = acc;
    app::job_pump::submit([&state, cap_acc, aid, oid, kind, from_received, partner]() mutable {
        auto run = [&](core::Account& a) {
            switch (kind) {
                case 'a': return trade::accept_trade_offer(a, oid, partner);
                case 'd': return trade::decline_trade_offer(a, oid);
                default:  return trade::cancel_trade_offer(a, oid);
            }
        };

        auto result = run(cap_acc);
        if (!result.ok && result.needs_relogin) {
            const auto cd = state.relogin_cooldown_seconds(aid);
            if (cd > 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "login rate-limited, try again in %lld s",
                              static_cast<long long>(cd));
                result.error = buf;
            } else {
                std::string relogin_err;
                if (state.auto_relogin(aid, cap_acc, &relogin_err)) {
                    apply_refreshed_tokens(state, cap_acc);
                    result = run(cap_acc);
                } else {
                    result.error = "auto-relogin failed: " + relogin_err;
                }
            }
        }

        // ensure_web_session may have re-minted the cookie/token; persist it.
        apply_refreshed_tokens(state, cap_acc);

        {
            std::lock_guard lk(g_mtx);
            g_in_flight.erase(oid);
        }

        const bool ok = result.ok;
        const bool needs_conf = result.needs_confirmation;
        const std::string err = result.error;
        state.post_ui_callback([&state, aid, oid, kind, from_received, ok, needs_conf, err] {
            const char* verb = kind == 'a' ? "accepted" : (kind == 'd' ? "declined" : "canceled");
            if (ok) {
                erase_offer(state, aid, oid, from_received);
                if (kind == 'a' && needs_conf) {
                    push_toast(state, "trade-" + oid,
                               "Trade accepted - confirm it in Confirmations", aid, false);
                    confirmations_trigger_refresh_all(state);
                } else {
                    push_toast(state, "trade-" + oid, std::string("Trade ") + verb, aid, false);
                }
            } else {
                {
                    std::lock_guard lk(g_mtx);
                    g_states[aid].last_error = err;
                }
                push_toast(state, "trade-" + oid,
                           std::string("Trade ") + verb + " failed: " + err, aid, true);
            }
        });
    });
}

void draw_item_strip(const std::vector<trade::InventoryItem>& items) {
    if (items.empty()) {
        ImGui::TextDisabled("  (nothing)");
        return;
    }
    auto* dl = ImGui::GetWindowDrawList();
    constexpr int kMaxShow = 8;
    int shown = 0;
    for (const auto& it : items) {
        if (shown >= kMaxShow) break;
        if (shown > 0) ImGui::SameLine(0.0F, 4.0F);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(kItemIcon, kItemIcon));
        dl->AddRectFilled(c, ImVec2(c.x + kItemIcon, c.y + kItemIcon),
                          ImGui::ColorConvertFloat4ToU32(theme::panel_hover()), 4.0F);
        if (auto* srv = widgets::avatar_for(economy_image_url(it.icon_url))) {
            dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv), c,
                                ImVec2(c.x + kItemIcon, c.y + kItemIcon),
                                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.0F);
        }
        if (ImGui::IsItemHovered() && !it.market_hash_name.empty())
            set_tooltip("%s", it.market_hash_name.c_str());
        ++shown;
    }
    if (static_cast<int>(items.size()) > kMaxShow) {
        ImGui::SameLine(0.0F, 6.0F);
        ImGui::Text("+%d", static_cast<int>(items.size()) - kMaxShow);
    }
}

enum class OfferAction { None, Accept, Decline, Cancel };

OfferAction draw_offer_card(const trade::TradeOffer& o, float width, bool in_flight) {
    OfferAction action = OfferAction::None;

    ImGui::PushID(o.offer_id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel());
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));

    ImGui::BeginChild("##offer", ImVec2(width, kCardHeight), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);

    char header[80];
    std::snprintf(header, sizeof(header), "Partner %llu",
                  static_cast<unsigned long long>(o.partner_steam_id_64));
    ImGui::TextUnformatted(header);

    {
        ImVec4 col = theme::dim_text();
        if (o.state == trade::TradeOfferState::Active) col = theme::accent();
        else if (o.state == trade::TradeOfferState::CreatedNeedsConfirmation ||
                 o.state == trade::TradeOfferState::InEscrow) col = theme::warning();
        ImGui::SameLine(ImGui::GetWindowSize().x - kStatePillW - 12.0F);
        draw_pill(trade::to_string(o.state), col, true, kStatePillW);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("You give:");
    draw_item_strip(o.items_to_give);
    ImGui::TextDisabled("You receive:");
    draw_item_strip(o.items_to_receive);

    if (o.escrow_end_unix > now_unix()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::Text("In escrow until %s", format_date(o.escrow_end_unix).c_str());
        ImGui::PopStyleColor();
    }

    ImGui::SetCursorPosY(ImGui::GetWindowSize().y - kButtonRowH);
    ImGui::BeginDisabled(in_flight);
    if (o.is_our_offer) {
        if (action_button(in_flight ? "Working..." : "Cancel", ImVec2(kBtnW, 0)))
            action = OfferAction::Cancel;
    } else {
        if (action_button(in_flight ? "Working..." : "Accept", ImVec2(kBtnW, 0)))
            action = OfferAction::Accept;
        ImGui::SameLine();
        if (action_button("Decline", ImVec2(kBtnW, 0)))
            action = OfferAction::Decline;
    }
    ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    return action;
}

void draw_offer_grid(app::AppState& state, core::Account& a,
                     const std::vector<const trade::TradeOffer*>& offers,
                     const std::unordered_set<std::string>& inflight) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1,
        static_cast<int>((avail + kGap) / (kCardMinWidth + kGap)));
    int col = 0;
    for (const auto* o : offers) {
        if (col > 0) ImGui::SameLine(0.0F, kGap);
        const float card_w = (avail - kGap * static_cast<float>(columns - 1)) /
                             static_cast<float>(columns);
        const bool busy = inflight.count(o->offer_id) != 0;
        const auto act = draw_offer_card(*o, card_w, busy);
        if (act == OfferAction::Accept)       submit_action(state, a, *o, 'a');
        else if (act == OfferAction::Decline) submit_action(state, a, *o, 'd');
        else if (act == OfferAction::Cancel)  submit_action(state, a, *o, 'c');
        col = (col + 1) % columns;
    }
}

// Still cancellable; terminal states (accepted/declined/canceled/expired/escrow) are hidden.
bool is_outgoing_actionable(trade::TradeOfferState s) {
    return s == trade::TradeOfferState::Active ||
           s == trade::TradeOfferState::CreatedNeedsConfirmation;
}

// False (account hidden) unless it has actionable offers, is refreshing, or has an error.
// Uses find() so the draw loop never inserts entries for never-refreshed accounts.
bool snapshot_for_draw(const std::string& aid, DrawSnap& out) {
    std::lock_guard lk(g_mtx);
    const auto it = g_states.find(aid);
    if (it == g_states.end()) return false;
    const auto& st = it->second;

    bool has_active_in = false;
    for (const auto& o : st.received) {
        if (o.state == trade::TradeOfferState::Active) { has_active_in = true; break; }
    }
    bool has_active_out = false;
    for (const auto& o : st.sent) {
        if (is_outgoing_actionable(o.state)) { has_active_out = true; break; }
    }
    const bool has_any = has_active_in || has_active_out;
    if (!has_any && !st.refreshing && st.last_error.empty()) return false;

    out.sent = st.sent;
    out.received = st.received;
    out.refreshing = st.refreshing;
    out.last_error = st.last_error;
    return true;
}

void refresh_all(app::AppState& state) {
    std::vector<core::Account> snaps;
    for (auto& a : state.vault.accounts) {
        if (!account_can_trade(a)) continue;
        if (refresh_cooldown_remaining(state, a.id) > 0) continue;
        snaps.push_back(a);
    }
    if (snaps.empty()) return;

    const auto stagger = state.settings.trade.refresh_stagger_ms;
    app::job_pump::submit([&state, snaps = std::move(snaps), stagger]() mutable {
        for (auto& a : snaps) {
            state.post_ui_callback([&state, a] { submit_account_refresh(state, a); });
            if (stagger > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(stagger));
        }
    });
}

void draw_account_section(app::AppState& state, core::Account& a,
                          const DrawSnap& snap,
                          const std::unordered_set<std::string>& inflight) {
    ImGui::PushID(a.id.c_str());

    const auto remaining = refresh_cooldown_remaining(state, a.id);
    ImGui::BeginDisabled(snap.refreshing || remaining > 0);
    if (action_button("Refresh", ImVec2(kBtnW * 0.8F, 0))) submit_account_refresh(state, a);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && remaining > 0) {
        set_tooltip("Wait %llds before refreshing again.",
                    static_cast<long long>(remaining));
    }
    ImGui::SameLine();
    if (action_button("Send offer", ImVec2(kBtnW, 0))) open_send_modal(state, a.id);

    if (snap.refreshing) {
        ImGui::SameLine();
        ImGui::TextDisabled("(refreshing)");
    } else if (!snap.last_error.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::Text("(%s)", snap.last_error.c_str());
        ImGui::PopStyleColor();
    }

    std::vector<const trade::TradeOffer*> incoming;
    std::vector<const trade::TradeOffer*> outgoing;
    for (const auto& o : snap.received) {
        if (o.state == trade::TradeOfferState::Active) incoming.push_back(&o);
    }
    for (const auto& o : snap.sent) {
        if (is_outgoing_actionable(o.state)) outgoing.push_back(&o);
    }

    ImGui::Spacing();
    if (!incoming.empty()) {
        ImGui::TextDisabled("Incoming (%zu)", incoming.size());
        draw_offer_grid(state, a, incoming, inflight);
        ImGui::Spacing();
    }
    if (!outgoing.empty()) {
        ImGui::TextDisabled("Outgoing (%zu)", outgoing.size());
        draw_offer_grid(state, a, outgoing, inflight);
        ImGui::Spacing();
    }
    if (incoming.empty() && outgoing.empty() && snap.last_error.empty() &&
        !snap.refreshing) {
        ImGui::TextDisabled("No active offers.");
    }

    ImGui::PopID();
    ImGui::Spacing();
}

}  // namespace

void draw_trade_offers(app::AppState& state) {
    using namespace trade_offers_detail;

    static std::string g_search;
    static bool g_history_open = false;

    ImGui::TextUnformatted("Trade Offers");
    ImGui::SameLine();
    if (action_button("Refresh all")) refresh_all(state);

    bool has_tradable = false;
    for (auto& a : state.vault.accounts) {
        if (account_can_trade(a)) { has_tradable = true; break; }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_tradable);
    if (action_button("Send offer")) open_send_modal(state, std::string{});
    ImGui::EndDisabled();

    ImGui::SameLine();
    const int incoming_total =
        state.pending_trade_offers_count.load(std::memory_order_relaxed);
    ImGui::BeginDisabled(incoming_total == 0);
    if (action_button("Accept all incoming")) {
        g_bulk_accept_count = incoming_total;
        g_confirm_accept_open = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (action_button("History")) g_history_open = true;

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Manual refresh - Steam rate-limits trades. Bulk actions run staggered.");

    ImGui::Spacing();
    widgets::draw_search_bar(g_search, 320.0F);
    ImGui::Spacing();

    std::string search_lower = g_search;
    for (char& c : search_lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto matches = [&](const core::Account& a) {
        if (search_lower.empty()) return true;
        auto contains = [&](const std::string& hay) {
            std::string lo = hay;
            for (char& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lo.find(search_lower) != std::string::npos;
        };
        return contains(a.login) || contains(a.web.persona_name);
    };

    std::unordered_set<std::string> inflight;
    {
        std::lock_guard lk(g_mtx);
        inflight = g_in_flight;
    }

    int sections = 0;
    int total_tradable = 0;
    for (auto& a : state.vault.accounts) {
        if (!account_can_trade(a)) continue;
        ++total_tradable;
        if (!matches(a)) continue;

        DrawSnap snap;
        if (!snapshot_for_draw(a.id, snap)) continue;

        separator_text(account_label(state, a).c_str());
        draw_account_section(state, a, snap, inflight);
        ++sections;
    }

    if (sections == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        if (total_tradable == 0) {
            ImGui::TextWrapped(
                "No tradable accounts. Trades need a full (password) login; "
                "token-only (NFA) accounts can't trade.");
        } else if (!search_lower.empty()) {
            ImGui::TextWrapped("No accounts with offers match \"%s\".", g_search.c_str());
        } else {
            ImGui::TextWrapped(
                "No active trade offers. Click \"Refresh all\" to check Steam, "
                "or \"Send offer\" to create one.");
        }
        ImGui::PopStyleColor();
    }

    draw_send_modal(state);
    draw_bulk_confirms(state);
    draw_history_modal(state, &g_history_open);
}

}  // namespace sam::ui::screens
