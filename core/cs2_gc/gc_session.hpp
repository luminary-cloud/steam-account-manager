#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/cs2_gc/so_cache.hpp"
#include "core/steam_cm/cm_session.hpp"

namespace google::protobuf {
class MessageLite;
}

namespace sam::cs2_gc {

struct GcNotification {
    std::uint32_t request = 0;
    std::vector<std::uint64_t> item_ids;
};

struct PersonalStore {
    bool loaded = false;
    std::uint32_t generation_time = 0;
    std::uint32_t redeemable_balance = 0;
    std::vector<std::uint64_t> items;
};

// CS2 account XP/level (from the GC player profile). Ranking up grants the weekly drop.
struct PlayerXp {
    bool valid = false;
    int level = 0;
    int cur_xp = 0;      // raw player_cur_xp (offset by a base; see ItemSchema consumer)
    int bonus_flags = 0;
};

// CS2 weekly recurring mission progress (from the GC SO cache). Per-account, keyed to a
// period; the name/target come from the mission schedule's templates, matched by id.
struct RecurringMission {
    bool valid = false;
    std::uint32_t mission_id = 0;
    std::uint32_t period = 0;
    std::uint32_t progress = 0;
};

// The current weekly mission, ready for display: the schedule template (name/target/xp)
// for the account's assigned mission, with live progress (0 when the account's mission SO
// belongs to an older period, i.e. nothing earned yet this week).
struct MissionInfo {
    bool valid = false;
    std::string name;          // e.g. "Competitive - Train - Round Wins"
    std::uint32_t progress = 0;
    std::uint32_t target = 0;   // rounds/kills to finish (the last point tier)
    std::uint32_t xp = 0;       // total XP across tiers
};

// Drives the CS2 Game Coordinator over a logged-on CmSession: announces the game,
// completes the GC hello/welcome handshake, and tracks the live SO cache (inventory
// + weekly personal store). Synchronous, single owning thread.
class GcSession {
public:
    // `abort` (optional) lets a caller interrupt pump loops promptly on teardown.
    explicit GcSession(steam_cm::CmSession& cm, const std::atomic<bool>* abort = nullptr)
        : cm_(cm), abort_(abort) {}

    // Sends games_played(730), waits out the GC's warm-up, then sends ClientHello on
    // a backoff until ClientWelcome arrives. Sets `error` and returns false on failure.
    bool launch(std::string& error);

    bool send_gc(std::uint32_t gc_emsg, const google::protobuf::MessageLite& body);
    bool pump(int timeout_ms);
    bool wait_until(const std::function<bool()>& pred, int timeout_ms);

    const std::unordered_map<std::uint64_t, EconItem>& inventory() const { return inventory_; }
    bool pop_notification(GcNotification& out);
    const PersonalStore& personal_store() const { return personal_store_; }
    const PlayerXp& player_xp() const { return player_xp_; }
    // The current weekly mission resolved for display (combines the account's mission SO
    // with the schedule template). `valid` is false until both have arrived.
    MissionInfo current_mission() const;
    // Monotonic counter bumped on every SO-cache change (inventory item add/remove/update
    // or personal-store update). Lets the owner re-snapshot when live GC pushes land.
    std::uint64_t so_seq() const { return so_seq_; }
    const std::string& items_game_url() const { return items_game_url_; }
    std::uint32_t item_schema_version() const { return item_schema_version_; }
    bool welcomed() const { return welcome_received_; }
    // True when the last launch() failed because the connection dropped, as opposed
    // to a clean welcome timeout; lets the client decide whether to retry.
    bool dropped() const { return dropped_; }

private:
    void on_cm_message(std::uint32_t emsg, const ::CMsgProtoBufHeader& header,
                       const std::string& body);
    void on_gc_message(std::uint32_t gc_emsg, const std::string& gc_body);
    void upsert_econ_item(const std::string& data);
    void erase_econ_item(const std::string& data);
    void load_personal_store(const std::string& data);
    // An SO object whose type id we don't model. Logs the type id once (the weekly
    // mission's id is undocumented) and identifies the recurring mission by shape.
    void on_unknown_so(int type_id, const std::string& data);
    void request_players_profile();
    void request_mission_schedule();
    void send_hello();
    void pump_for(int total_ms);

    steam_cm::CmSession& cm_;
    const std::atomic<bool>* abort_ = nullptr;
    bool welcome_received_ = false;
    bool dropped_ = false;
    std::unordered_map<std::uint64_t, EconItem> inventory_;
    std::uint64_t so_seq_ = 0;
    std::deque<GcNotification> notifications_;
    PersonalStore personal_store_;
    PlayerXp player_xp_;
    RecurringMission recurring_mission_;
    std::string current_period_templates_;     // raw template blob for the active period
    std::uint32_t current_period_ = 0;         // schedule period (unix start) containing now
    std::unordered_set<int> logged_so_types_;  // SO type ids already logged (discovery aid)
    int recurring_so_type_ = -1;               // discovered type id carrying the mission
    std::string items_game_url_;
    std::uint32_t item_schema_version_ = 0;
};

}  // namespace sam::cs2_gc
