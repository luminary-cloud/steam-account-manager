#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/cs2_gc/cs2_gc_client.hpp"

namespace sam::ui::screens {

// Per-account state for the CS2 screen. Display fields are written only on the UI
// thread (from post_ui_callback lambdas) so they need no locking. `client` is last
// so it is torn down (thread joined) before the rest on destruction.
struct Cs2ScreenState {
    std::string account_id;
    std::string status;
    std::string error;
    bool connected = false;
    bool action_busy = false;
    bool acquiring = false;  // minting the SteamClient token

    cs2_gc::Snapshot snapshot;

    std::uint64_t open_unit = 0;
    bool unit_loading = false;
    std::vector<cs2_gc::DisplayItem> unit_contents;

    // Grid selection (by item id, survives page changes) + per-grid page index.
    std::unordered_set<std::uint64_t> selected_inv;
    std::unordered_set<std::uint64_t> selected_unit;
    std::unordered_set<std::uint64_t> selected_reward;  // weekly-drop items picked to claim
    int inv_page = 0;
    int units_page = 0;
    int contents_page = 0;
    int reward_page = 0;

    std::unique_ptr<cs2_gc::Cs2GcClient> client;
};

}  // namespace sam::ui::screens
