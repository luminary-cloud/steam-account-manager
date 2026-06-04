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
#include "core/trade/actions.hpp"
#include "core/trade/inventory.hpp"
#include "core/trade/offers.hpp"
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

constexpr float kCardMinWidth = 360.0F;
constexpr float kCardHeight   = 210.0F;
constexpr float kGap          = 14.0F;
constexpr float kButtonRowH   = 36.0F;
constexpr float kItemIcon     = 40.0F;
constexpr float kBtnW         = 112.0F;
constexpr float kStatePillW   = 132.0F;
constexpr float kTile         = 56.0F;   // inventory tile box
constexpr float kIconInset    = 7.0F;    // icon padding inside the tile box

struct AccountTradeState {
    std::vector<trade::TradeOffer> sent;
    std::vector<trade::TradeOffer> received;
    bool refreshing = false;
    std::string last_error;
    std::int64_t last_refresh_unix = 0;

    std::vector<trade::InventoryItem> inventory;
    bool inv_loading = false;
    std::string inv_error;
    std::int64_t inv_loaded_unix = 0;
};

// Lightweight per-frame copy for drawing: just the offer lists, never the
// inventory. Keeps the 100-account list cheap to render.
struct DrawSnap {
    std::vector<trade::TradeOffer> sent;
    std::vector<trade::TradeOffer> received;
    bool refreshing = false;
    std::string last_error;
};

std::mutex g_mtx;
std::unordered_map<std::string, AccountTradeState> g_states;
std::unordered_set<std::string> g_in_flight;  // offer ids with an action running

// One tradable item in the picker, with its name pre-lowercased for filtering.
struct PickItem {
    trade::InventoryItem item;
    std::string lname;
};

// State for the create-trade modal. UI-thread only. One account selected =
// pick specific items; several selected = bulk send-all-tradable.
struct SendModal {
    bool open_request = false;
    std::unordered_set<std::string> accounts;  // selected source accounts
    char url_buf[256] = {};
    char msg_buf[160] = {};
    char acct_search[64] = {};                 // account-list filter
    char search_buf[64] = {};                  // item filter
    std::unordered_set<std::uint64_t> selected;  // chosen item asset ids (single mode)
    bool bulk_ack = false;                      // "I understand" for bulk send

    // Cache of the single picked account's tradable inventory, rebuilt only when
    // the underlying inventory changes (not every frame).
    std::vector<PickItem> inv_cache;
    std::int64_t inv_cache_unix = -1;
    std::string inv_cache_aid;
};
SendModal g_send;

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string account_label(app::AppState& state, const core::Account& a) {
    return a.web.persona_name.empty() ? widgets::login_label(state, a)
                                      : a.web.persona_name;
}

// A full web trade needs a password-backed session. NFA token-injection
// accounts get AccessDenied on community endpoints, so trades are unavailable.
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

void push_toast(app::AppState& state, const std::string& id, const std::string& msg,
                const std::string& account_id, bool warning) {
    ui::widgets::ToastItem t;
    t.id = id;
    t.message = msg;
    t.account_id = account_id;
    t.is_warning = warning;
    t.expires_at_unix = now_unix() + state.settings.notifications.toast_duration_seconds;
    state.toasts.push(std::move(t));
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

// Returns false (account hidden) unless it has offers, is refreshing, or has an
// error worth showing - mirroring the Confirmations page. Uses find() so the
// 100-account draw loop never inserts entries for never-refreshed accounts.
bool snapshot_for_draw(const std::string& aid, DrawSnap& out) {
    std::lock_guard lk(g_mtx);
    const auto it = g_states.find(aid);
    if (it == g_states.end()) return false;
    const auto& st = it->second;

    bool has_active_in = false;
    for (const auto& o : st.received) {
        if (o.state == trade::TradeOfferState::Active) { has_active_in = true; break; }
    }
    const bool has_any = has_active_in || !st.sent.empty();
    if (!has_any && !st.refreshing && st.last_error.empty()) return false;

    out.sent = st.sent;
    out.received = st.received;
    out.refreshing = st.refreshing;
    out.last_error = st.last_error;
    return true;
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
    const bool auto_conf = state.settings.trade.auto_confirm_sent;
    auto cap = acc;
    app::job_pump::submit([&state, cap, aid, tu, give, message, auto_conf]() mutable {
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
            state.post_ui_callback([&state, aid, err] {
                push_toast(state, "send-fail-" + aid, "Send failed: " + err, aid, true);
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

        state.post_ui_callback([&state, aid, oid, needs_conf, auto_conf, confirmed, conf_note] {
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
            // Force the new outgoing offer to show on the next refresh.
            {
                std::lock_guard lk(g_mtx);
                g_states[aid].last_refresh_unix = 0;
            }
            if (auto* a = state.find_account(aid)) submit_account_refresh(state, *a);
        });
    });
}

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

        // Web actions re-mint the session cookie via ensure_web_session, so
        // persist the refreshed token/cookie/session id.
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

// Opens the send dialog. account_id empty -> no preselection (header "Send
// offer"); otherwise preselect just that account.
void open_send_modal(app::AppState& state, const std::string& account_id) {
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
    for (const auto& o : snap.sent) outgoing.push_back(&o);

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

void bulk_send_all(app::AppState& state, std::vector<core::Account> accs,
                   trade::TradeUrl tu);

void draw_send_modal(app::AppState& state) {
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

    // Layout: the inventory grid is fixed at 3 rows; the account grid flexes to
    // fill everything else, so any extra/resized height lands on account selection.
    const float kLine  = ImGui::GetTextLineHeightWithSpacing();
    const float kFrame = ImGui::GetFrameHeightWithSpacing();
    constexpr float kChipH   = 44.0F;
    constexpr float kChipGap = 8.0F;
    const float items_h = 3.0F * (kTile + ImGui::GetStyle().ItemSpacing.y) + 10.0F;
    // The footer (message + Send/Cancel) is pinned to the bottom of the modal; the
    // account grid flexes to fill the space above the fixed middle, so the popup
    // never scrolls and the buttons always sit flush at the bottom.
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

    // --- Section 1: source accounts (flexible height) ---
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

    const std::string af = lower_copy(g_send.acct_search);
    std::vector<const core::Account*> accts;
    for (auto& a : state.vault.accounts) {
        if (!account_can_trade(a)) continue;
        if (!af.empty() &&
            lower_copy(account_label(state, a)).find(af) == std::string::npos &&
            lower_copy(a.login).find(af) == std::string::npos) continue;
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
    if (!saved.empty()) {
        ImGui::SetNextItemWidth(150.0F);
        if (begin_styled_combo("##saved", "Saved links")) {
            for (const auto& s : saved) {
                if (ImGui::Selectable(s.c_str()))
                    std::snprintf(g_send.url_buf, sizeof(g_send.url_buf), "%s", s.c_str());
            }
            end_styled_combo();
        }
        ImGui::SameLine();
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
            const std::string s = g_send.url_buf;
            if (std::find(saved.begin(), saved.end(), s) == saved.end()) saved.push_back(s);
            state.settings.trade.default_destination_trade_url = s;
            state.save_settings();
        }
    } else if (g_send.url_buf[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextUnformatted("Invalid trade link");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    bool can_send = false;

    if (!single.empty()) {
        // Single account: pick specific items.
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
                    g_send.inv_cache.push_back({it, lower_copy(it.market_hash_name)});
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
        const std::string filt = lower_copy(g_send.search_buf);
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
        // Justify: spread the leftover width into the gaps so square tiles fill
        // the row edge-to-edge (no dead space on the right).
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
            // Clipper so only on-screen tiles draw (and download icons).
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
            const std::string s = g_send.url_buf;
            if (std::find(saved.begin(), saved.end(), s) == saved.end()) saved.push_back(s);
            state.settings.trade.default_destination_trade_url = s;
            state.save_settings();
            submit_send(state, *acc, tu, std::move(give), g_send.msg_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
    } else {
        // Bulk (2+) or nothing selected: no item picker.
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
            const std::string s = g_send.url_buf;
            if (std::find(saved.begin(), saved.end(), s) == saved.end()) saved.push_back(s);
            state.settings.trade.default_destination_trade_url = s;
            state.save_settings();
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

// Runs `fn(creds)` once; on a session error, attempts a single auto-relogin
// (respecting its cooldown) and retries. Works for any result type with `ok`
// and `needs_relogin` members (accept/send/inventory results).
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

bool g_confirm_accept_open = false;
int  g_bulk_accept_count = 0;

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

    const int stagger = std::max(1500, state.settings.trade.refresh_stagger_ms);
    app::job_pump::submit([&state, targets = std::move(targets), stagger]() mutable {
        int ok = 0;
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
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(stagger));
        }
        const int total = static_cast<int>(targets.size());
        state.post_ui_callback([&state, ok, total] {
            push_toast(state, "bulk-accept",
                       "Bulk accept: " + std::to_string(ok) + "/" +
                           std::to_string(total) + " offers accepted",
                       "", ok != total);
        });
    });
}

void bulk_send_all(app::AppState& state, std::vector<core::Account> accs,
                   trade::TradeUrl tu) {
    if (!tu.ok || accs.empty()) return;

    const int stagger = std::max(2000, state.settings.trade.refresh_stagger_ms);
    const bool auto_conf = state.settings.trade.auto_confirm_sent;
    app::job_pump::submit([&state, accs = std::move(accs), tu, stagger, auto_conf]() mutable {
        int sent_ok = 0;
        int attempted = 0;
        for (auto& seed : accs) {
            core::Account creds = seed;
            auto inv = with_relogin(state, creds, [&](core::Account& a) {
                return trade::fetch_inventory(a, a.steam_id_64, 730, 2, 2000, 0);
            });
            if (tokens_changed(seed, creds)) apply_refreshed_tokens(state, creds);
            if (!inv.ok) continue;

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
            if (give.empty()) continue;

            ++attempted;
            auto res = with_relogin(state, creds, [&](core::Account& a) {
                return trade::send_trade_offer(a, tu, give, "");
            });
            apply_refreshed_tokens(state, creds);
            if (res.ok) {
                ++sent_ok;
                if (res.needs_confirmation && auto_conf) {
                    for (int att = 0; att < 3; ++att) {
                        if (att > 0)
                            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                        const auto cr = trade::confirm_sent_offer(creds, res.offer_id);
                        if (cr.ok || !cr.not_found) break;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(stagger));
        }
        state.post_ui_callback([&state, sent_ok, attempted] {
            push_toast(state, "bulk-send",
                       "Bulk send: " + std::to_string(sent_ok) + "/" +
                           std::to_string(attempted) + " accounts sent",
                       "", sent_ok != attempted);
            confirmations_trigger_refresh_all(state);
        });
    });
}

void draw_bulk_confirms(app::AppState& state) {
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

}  // namespace

void draw_trade_offers(app::AppState& state) {
    static std::string g_search;

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
        if (!snapshot_for_draw(a.id, snap)) continue;  // hide idle accounts

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
}

}  // namespace sam::ui::screens
