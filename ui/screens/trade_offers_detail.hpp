#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"
#include "core/trade/inventory.hpp"
#include "core/trade/models.hpp"
#include "core/trade/offers.hpp"
#include "core/trade/trade_url.hpp"

namespace sam::ui::screens {
namespace trade_offers_detail {

constexpr float kCardMinWidth = 360.0F;
constexpr float kCardHeight   = 234.0F;
constexpr float kGap          = 14.0F;
constexpr float kButtonRowH   = 36.0F;
constexpr float kItemIcon     = 40.0F;
constexpr float kBtnW         = 112.0F;
constexpr float kStatePillW   = 132.0F;
constexpr float kTile         = 56.0F;
constexpr float kIconInset    = 7.0F;

struct AccountTradeState {
    std::vector<core::trade::TradeOffer> sent;
    std::vector<core::trade::TradeOffer> received;
    bool refreshing = false;
    std::string last_error;
    std::int64_t last_refresh_unix = 0;

    std::vector<core::trade::InventoryItem> inventory;
    bool inv_loading = false;
    std::string inv_error;
    std::int64_t inv_loaded_unix = 0;
};

// Per-frame draw copy: offer lists only, never inventory, to keep the 100-account list cheap.
struct DrawSnap {
    std::vector<core::trade::TradeOffer> sent;
    std::vector<core::trade::TradeOffer> received;
    bool refreshing = false;
    std::string last_error;
};

struct PickItem {
    core::trade::InventoryItem item;
    std::string lname;  // pre-lowercased for filtering
};

// UI-thread only. One account selected = pick specific items; several = bulk send-all-tradable.
struct SendModal {
    bool open_request = false;
    std::unordered_set<std::string> accounts;
    char url_buf[256] = {};
    char msg_buf[160] = {};
    char acct_search[64] = {};
    char search_buf[64] = {};
    std::unordered_set<std::uint64_t> selected;  // chosen item asset ids (single mode)
    bool bulk_ack = false;

    // Saved-link rename sub-modal. Keyed by url so list edits don't invalidate the target.
    std::string rename_url;
    char rename_buf[128] = {};
    bool rename_open_request = false;

    // Rebuilt only when the underlying inventory changes, not every frame.
    std::vector<PickItem> inv_cache;
    std::int64_t inv_cache_unix = -1;
    std::string inv_cache_aid;
};

// Shared persistent state: exactly one instance across both translation units.
inline std::mutex g_mtx;
inline std::unordered_map<std::string, AccountTradeState> g_states;
inline std::unordered_set<std::string> g_in_flight;  // offer ids with an action running
inline SendModal g_send;
inline bool g_confirm_accept_open = false;
inline int  g_bulk_accept_count = 0;

// Shared helpers, defined in trade_offers_screen.cpp.
std::int64_t now_unix();
std::string account_label(app::AppState& state, const core::Account& a);
bool account_can_trade(const core::Account& a);
std::string economy_image_url(const std::string& icon_url);
std::string format_date(std::int64_t unix_s);
void apply_refreshed_tokens(app::AppState& state, const core::Account& creds);
bool tokens_changed(const core::Account& before, const core::Account& after);
void push_toast(app::AppState& state, const std::string& id, const std::string& msg,
                const std::string& account_id, bool warning);
void push_toast_at(app::AppState& state, const std::string& id, const std::string& msg,
                   const std::string& account_id, bool warning, std::int64_t expires_at);
void recount_incoming(app::AppState& state);
void erase_offer(app::AppState& state, const std::string& account_id,
                 const std::string& offer_id, bool from_received);
std::int64_t cooldown_remaining(std::int64_t last, int window);
std::int64_t refresh_cooldown_remaining(const app::AppState& state, const std::string& aid);
void submit_account_refresh(app::AppState& state, const core::Account& seed);
void submit_inventory_load(app::AppState& state, const core::Account& seed);
void submit_send(app::AppState& state, const core::Account& acc, core::trade::TradeUrl tu,
                 std::vector<core::trade::TradeAssetRef> give, std::string message);

// Modal + card-rendering cluster, defined in trade_offers_modals.cpp.
void open_send_modal(app::AppState& state, const std::string& account_id);
void draw_send_modal(app::AppState& state);
void draw_bulk_confirms(app::AppState& state);
void draw_history_modal(app::AppState& state, bool* p_open);

}  // namespace trade_offers_detail
}  // namespace sam::ui::screens
