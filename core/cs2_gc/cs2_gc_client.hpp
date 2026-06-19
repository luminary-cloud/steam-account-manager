#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/cs2_gc/item_schema.hpp"

namespace sam::cs2_gc {

class GcSession;

struct DisplayItem {
    std::uint64_t id = 0;
    std::string name;
    std::string icon_url;       // full Steam CDN icon URL, empty when unknown
    std::uint32_t border_rgb = 0;  // rarity/grade color packed 0xRRGGBB, 0 = neutral
    std::uint64_t casket_id = 0;
    bool is_storage_unit = false;
    int item_count = -1;  // storage units only
};

struct WeeklyReward {
    bool loaded = false;  // the GC has sent the personal store (else still connecting)
    bool available = false;
    std::uint32_t redeemable_balance = 0;
    std::uint32_t generation_time = 0;  // unix time the current store was generated
    std::vector<std::uint64_t> item_ids;
    std::vector<DisplayItem> items;  // resolved candidates for display/selection
    bool claimed = false;            // this period's pick is known (the drop was claimed)
    std::vector<std::string> claimed_names;  // names of the items picked this period
};

// Account XP toward the next level; ranking up grants the weekly drop.
struct PlayerProgress {
    bool valid = false;
    int level = 0;
    int xp_in_level = 0;       // 0 .. xp_per_level
    int xp_per_level = 5000;
};

// The current weekly recurring mission. progress/target are e.g. rounds won; target is 0
// when the schema gives none (show the raw count instead of a bar). xp is the total reward.
struct WeeklyMission {
    bool valid = false;
    std::uint32_t progress = 0;
    std::uint32_t target = 0;
    std::uint32_t xp = 0;
    std::string name;
};

struct Snapshot {
    std::vector<DisplayItem> items;  // main inventory, movable into a unit
    std::vector<DisplayItem> units;  // storage units
    WeeklyReward reward;
    PlayerProgress progress;
    WeeklyMission mission;
};

struct Cs2Credentials {
    std::string refresh_token;  // must carry the "client" audience
    std::uint64_t steam_id = 0;
    std::string proxy;  // per-account proxy url, honored under ProxyMode::PerAccount
    // Last observed weekly-drop claim, seeded from the vault so a reconnect can show what
    // was picked this period without re-claiming.
    std::uint32_t claimed_generation = 0;
    std::vector<std::uint64_t> claimed_ids;
    std::vector<std::string> claimed_names;
};

// All callbacks run on the worker thread; the UI layer wraps them to marshal onto
// the render thread.
struct Cs2Callbacks {
    std::function<void(std::string)> on_status;
    std::function<void(Snapshot)> on_snapshot;
    std::function<void(std::uint64_t unit_id, std::vector<DisplayItem>)> on_unit_contents;
    std::function<void(std::string)> on_error;
    std::function<void(std::string)> on_action_done;
    // Fired after a weekly drop is claimed so the UI can cache what was picked (the
    // generation it applies to, the item ids, and their resolved names).
    std::function<void(std::uint32_t generation_time, std::vector<std::uint64_t> ids,
                       std::vector<std::string> names)>
        on_reward_claimed;
};

// Owns a warm CM + GC session for one account on a dedicated thread. Connects on
// construction, services move/claim commands, and tears down on stop()/destruction.
class Cs2GcClient {
public:
    Cs2GcClient(Cs2Credentials creds, Cs2Callbacks cb);
    ~Cs2GcClient();
    Cs2GcClient(const Cs2GcClient&) = delete;
    Cs2GcClient& operator=(const Cs2GcClient&) = delete;

    void load_unit(std::uint64_t unit_id);
    void move_in(std::uint64_t unit_id, std::uint64_t item_id);
    void move_out(std::uint64_t unit_id, std::uint64_t item_id);
    // Batch variants: move every id, then post a single snapshot + done callback.
    void move_in_many(std::uint64_t unit_id, std::vector<std::uint64_t> item_ids);
    void move_out_many(std::uint64_t unit_id, std::vector<std::uint64_t> item_ids);
    void claim_reward(std::vector<std::uint64_t> item_ids);
    // Re-posts a snapshot of the current SO cache (the GC keeps it live, so this just
    // pushes the latest state to the UI). Live changes auto-refresh too; this is manual.
    void refresh();
    // Signals the worker to wind down without blocking; the join still happens in the
    // destructor. Use retire() to destroy a client off the UI thread.
    void begin_stop();
    void stop();

private:
    enum class Op { LoadUnit, MoveIn, MoveOut, MoveInMany, MoveOutMany, ClaimReward, Refresh };
    struct Command {
        Op op;
        std::uint64_t unit_id = 0;
        std::uint64_t item_id = 0;
        std::vector<std::uint64_t> items;
    };

    void run();
    void enqueue(Command c);
    void post_snapshot(GcSession& gc);

    Cs2Credentials creds_;
    Cs2Callbacks cb_;
    std::shared_ptr<const ItemSchema> schema_;

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    std::atomic<bool> stop_{false};
    std::uint64_t last_so_seq_ = 0;  // SO seq last reflected in a posted snapshot
    // Names of the items this account picked for `redeemed_gen_time_`'s generation, seeded
    // from the vault and set on claim. Used to label "Claimed this week" (the picks become
    // owned items not in the store list, so their faux store ids can't be resolved later).
    std::vector<std::string> redeemed_names_;
    std::uint32_t redeemed_gen_time_ = 0;
};

// Destroys a client (joining its worker thread) on a background reaper so the caller
// never blocks on an in-flight connect/teardown. Safe to call from the UI thread.
void retire(std::unique_ptr<Cs2GcClient> client);

// Drains the reaper, joining any retired clients. Call once at shutdown, after
// http::cancel_all(), while AppState is still alive.
void reap_all();

}  // namespace sam::cs2_gc
