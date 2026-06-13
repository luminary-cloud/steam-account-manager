#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"
#include "core/sda/confirmation.hpp"

namespace sam::ui::screens {
namespace confirmations_detail {

constexpr float kCardMinWidth = 280.0F;
constexpr float kCardHeight   = 170.0F;
constexpr float kGap          = 14.0F;
constexpr float kGridInset    = 10.0F;
constexpr float kButtonRowH   = 40.0F;
constexpr float kIconSize     = 48.0F;
constexpr float kPillWidth    = 72.0F;
constexpr float kBulkButtonW  = 140.0F;

struct PendingList {
    std::vector<sda::Confirmation> items;
    bool refreshing = false;
    std::string last_error;
};

struct ScanResult {
    int targeted = 0;
    int linked = 0;
    int skipped_encrypted = 0;
    int skipped_no_match = 0;
    int skipped_ambiguous = 0;
    int parse_errors = 0;
};

enum class CardAction { None, Allow, Deny };

// Shared persistent state: exactly one instance across both translation units.
inline std::mutex g_mtx;
inline std::unordered_map<std::string, PendingList> g_lists;
inline std::atomic<bool> g_scan_in_progress{false};
inline bool g_show_scan_modal = false;
inline ScanResult g_last_scan;

// Shared helpers, defined in confirmations_screen.cpp.
void submit_account_refresh(app::AppState& state, const core::Account& seed);

// maFile linking + scan + card-rendering + modal cluster, defined in confirmations_modals.cpp.
bool needs_mafile_data(const core::Account& a);
void link_mafile_for_account(app::AppState& state, const core::Account& a);
void scan_folder_for_mafiles(app::AppState& state);
CardAction draw_confirmation_card(const sda::Confirmation& c, float width,
                                   bool* selected);
void draw_scan_result_modal();
void draw_history_modal(app::AppState& state, bool* p_open);

}  // namespace confirmations_detail
}  // namespace sam::ui::screens
