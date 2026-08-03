#include "core/cs2_gc/cs2_gc_client.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include "core/cs2_gc/casket_ops.hpp"
#include "core/cs2_gc/free_reward.hpp"
#include "core/cs2_gc/gc_session.hpp"
#include "core/http/client.hpp"
#include "core/log.hpp"
#include "core/steam_cm/cm_session.hpp"

namespace sam::cs2_gc {

namespace {

PlayerProgress make_progress(const PlayerXp& xp) {
    PlayerProgress p;
    if (!xp.valid) return p;
    constexpr int kXpBase = 327680000;
    constexpr int kXpPerLevel = 5000;
    int into = xp.cur_xp - kXpBase;
    if (into < 0) into = 0;
    p.valid = true;
    p.level = xp.level;
    p.xp_per_level = kXpPerLevel;
    p.xp_in_level = into % kXpPerLevel;
    return p;
}

std::vector<DisplayItem> resolve_medals(const ItemSchema& schema,
                                        const std::vector<std::uint32_t>& defidx) {
    std::vector<DisplayItem> out;
    out.reserve(defidx.size());
    for (std::uint32_t def : defidx) {
        EconItem fake;
        fake.def_index = def;
        const ResolvedItem r = schema.resolve(fake);
        DisplayItem d;
        d.id = def;
        d.name = r.name;
        d.icon_url = r.icon_url;
        d.border_rgb = r.border_rgb;
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace

Cs2GcClient::Cs2GcClient(Cs2Credentials creds, Cs2Callbacks cb)
    : creds_(std::move(creds)), cb_(std::move(cb)) {
    thread_ = std::thread([this] { run(); });
}

Cs2GcClient::~Cs2GcClient() { stop(); }

void Cs2GcClient::begin_stop() {
    stop_.store(true);
    cv_.notify_one();
}

void Cs2GcClient::stop() {
    begin_stop();
    if (thread_.joinable()) thread_.join();
}

void Cs2GcClient::enqueue(Command c) {
    {
        std::lock_guard lk(mtx_);
        commands_.push_back(std::move(c));
    }
    cv_.notify_one();
}

void Cs2GcClient::load_unit(std::uint64_t unit_id) { enqueue({Op::LoadUnit, unit_id, 0, {}}); }

void Cs2GcClient::move_in(std::uint64_t unit_id, std::uint64_t item_id) {
    enqueue({Op::MoveIn, unit_id, item_id, {}});
}

void Cs2GcClient::move_out(std::uint64_t unit_id, std::uint64_t item_id) {
    enqueue({Op::MoveOut, unit_id, item_id, {}});
}

void Cs2GcClient::move_in_many(std::uint64_t unit_id, std::vector<std::uint64_t> item_ids) {
    enqueue({Op::MoveInMany, unit_id, 0, std::move(item_ids)});
}

void Cs2GcClient::move_out_many(std::uint64_t unit_id, std::vector<std::uint64_t> item_ids) {
    enqueue({Op::MoveOutMany, unit_id, 0, std::move(item_ids)});
}

void Cs2GcClient::claim_reward(std::vector<std::uint64_t> item_ids) {
    enqueue({Op::ClaimReward, 0, 0, std::move(item_ids)});
}

void Cs2GcClient::refresh() { enqueue({Op::Refresh, 0, 0, {}}); }

void Cs2GcClient::pull_profiles(std::vector<std::uint32_t> account_ids) {
    std::vector<std::uint64_t> ids(account_ids.begin(), account_ids.end());
    enqueue({Op::PullProfiles, 0, 0, std::move(ids)});
}

void Cs2GcClient::post_snapshot(GcSession& gc) {
    Snapshot snap;

    const PersonalStore& ps = gc.personal_store();
    const bool reward_pending = ps.loaded && ps.redeemable_balance > 0 && !ps.items.empty();

    std::unordered_set<std::uint64_t> hide_from_inv;
    if (ps.loaded) hide_from_inv.insert(ps.items.begin(), ps.items.end());

    for (const auto& [id, it] : gc.inventory()) {
        if (it.is_storage_unit) {
            DisplayItem d;
            d.id = id;
            d.is_storage_unit = true;
            d.item_count = it.casket_item_count;
            d.name = it.custom_name.empty() ? "Storage Unit" : it.custom_name;
            snap.units.push_back(std::move(d));
        } else if (it.casket_id == 0) {
            if (hide_from_inv.count(id) != 0) continue;
            const ResolvedItem r = schema_->resolve(it);
            if (!r.storable) continue;
            DisplayItem d;
            d.id = id;
            d.name = r.name;
            d.icon_url = r.icon_url;
            d.border_rgb = r.border_rgb;
            snap.items.push_back(std::move(d));
        }
    }
    std::sort(snap.items.begin(), snap.items.end(),
              [](const DisplayItem& a, const DisplayItem& b) { return a.name < b.name; });
    std::sort(snap.units.begin(), snap.units.end(),
              [](const DisplayItem& a, const DisplayItem& b) { return a.name < b.name; });

    snap.reward.loaded = ps.loaded;
    snap.reward.available = reward_pending;
    snap.reward.redeemable_balance = ps.redeemable_balance;
    snap.reward.generation_time = ps.generation_time;
    snap.reward.item_ids = ps.items;
    if (reward_pending)
        for (std::uint64_t rid : ps.items) {
            const auto it = gc.inventory().find(rid);
            if (it == gc.inventory().end()) continue;
            const ResolvedItem r = schema_->resolve(it->second);
            DisplayItem d;
            d.id = rid;
            d.name = r.name;
            d.icon_url = r.icon_url;
            d.border_rgb = r.border_rgb;
            snap.reward.items.push_back(std::move(d));
        }

    const bool have_record = redeemed_gen_time_ == ps.generation_time && !redeemed_names_.empty();
    if (have_record) snap.reward.claimed_names = redeemed_names_;

    const PlayerXp& xp = gc.player_xp();
    snap.progress = make_progress(xp);
    snap.featured_medal_defidx = xp.featured_medal_defidx;
    snap.medals = resolve_medals(*schema_, xp.medal_defidx);

    const MissionInfo mi = gc.current_mission();
    if (mi.valid) {
        snap.mission.valid = true;
        snap.mission.name = mi.name;
        snap.mission.progress = mi.progress;
        snap.mission.target = mi.target;
        snap.mission.xp = mi.xp;
    }

    if (cb_.on_snapshot) cb_.on_snapshot(std::move(snap));
    last_so_seq_ = gc.so_seq();
}

void Cs2GcClient::run() {
    http::ScopedProxy proxy_guard(creds_.proxy);

    std::string err;
    std::unique_ptr<steam_cm::CmSession> cm_ptr;
    std::unique_ptr<GcSession> gc_ptr;
    constexpr int kMaxAttempts = 3;
    bool established = false;
    for (int attempt = 1; attempt <= kMaxAttempts && !stop_.load(); ++attempt) {
        if (cb_.on_status)
            cb_.on_status(attempt == 1 ? "Signing in to Steam"
                                       : "CS2 busy on another session, retrying (" +
                                             std::to_string(attempt) + "/" +
                                             std::to_string(kMaxAttempts) + ")");
        gc_ptr.reset();
        cm_ptr = std::make_unique<steam_cm::CmSession>(&stop_);
        if (cm_ptr->connect(creds_.refresh_token, creds_.steam_id, err)) {
            if (cb_.on_status)
                cb_.on_status("Connecting to CS2 (taking over the game session, up to a minute)");
            gc_ptr = std::make_unique<GcSession>(*cm_ptr, &stop_);
            if (gc_ptr->launch(err)) {
                established = true;
                break;
            }
        }
        cm_ptr->disconnect();

        const bool retryable = cm_ptr->last_logoff_eresult() == 6 || (gc_ptr && gc_ptr->dropped());
        if (!retryable || attempt == kMaxAttempts) break;
        for (int i = 0; i < 6 && !stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (cb_.on_logon && cm_ptr) cb_.on_logon(cm_ptr->logon_eresult());

    if (stop_.load()) return;
    if (!established) {
        if (cb_.on_error) cb_.on_error("CS2 unavailable: " + err);
        return;
    }

    steam_cm::CmSession& cm = *cm_ptr;
    GcSession& gc = *gc_ptr;
    schema_ = ItemSchema::shared(gc.item_schema_version(), gc.items_game_url());

    redeemed_gen_time_ = creds_.claimed_generation;
    redeemed_names_ = creds_.claimed_names;
    post_snapshot(gc);
    if (cb_.on_status) cb_.on_status("Ready");

    while (!stop_.load()) {
        Command cmd;
        bool have = false;
        {
            std::unique_lock lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(400),
                         [this] { return stop_.load() || !commands_.empty(); });
            if (stop_.load()) break;
            if (!commands_.empty()) {
                cmd = std::move(commands_.front());
                commands_.pop_front();
                have = true;
            }
        }
        if (!have) {
            if (!gc.pump(400)) {
                if (cb_.on_error) cb_.on_error("Connection to Steam was lost");
                break;
            }

            if (gc.so_seq() != last_so_seq_) post_snapshot(gc);
            continue;
        }

        switch (cmd.op) {
            case Op::LoadUnit: {
                std::vector<EconItem> items;
                if (sam::cs2_gc::load_unit(gc, cmd.unit_id, items, err)) {
                    std::vector<DisplayItem> out;
                    out.reserve(items.size());
                    for (const auto& it : items) {
                        const ResolvedItem r = schema_->resolve(it);
                        DisplayItem d;
                        d.id = it.id;
                        d.name = r.name;
                        d.icon_url = r.icon_url;
                        d.border_rgb = r.border_rgb;
                        d.casket_id = it.casket_id;
                        out.push_back(std::move(d));
                    }
                    std::sort(out.begin(), out.end(), [](const DisplayItem& a, const DisplayItem& b) {
                        return a.name < b.name;
                    });
                    if (cb_.on_unit_contents) cb_.on_unit_contents(cmd.unit_id, std::move(out));
                } else if (cb_.on_error) {
                    cb_.on_error(err);
                }
                break;
            }
            case Op::MoveIn:
                if (sam::cs2_gc::move_in(gc, cmd.unit_id, cmd.item_id, err)) {
                    if (cb_.on_action_done) cb_.on_action_done("Moved into the storage unit");
                } else if (cb_.on_error) {
                    cb_.on_error(err);
                }
                post_snapshot(gc);
                break;
            case Op::MoveOut:
                if (sam::cs2_gc::move_out(gc, cmd.unit_id, cmd.item_id, err)) {
                    if (cb_.on_action_done) cb_.on_action_done("Moved out of the storage unit");
                } else if (cb_.on_error) {
                    cb_.on_error(err);
                }
                post_snapshot(gc);
                break;
            case Op::MoveInMany: {
                int ok = 0;
                for (std::uint64_t item : cmd.items) {
                    if (stop_.load()) break;
                    if (sam::cs2_gc::move_in(gc, cmd.unit_id, item, err)) ++ok;
                }
                post_snapshot(gc);
                if (cb_.on_action_done)
                    cb_.on_action_done("Stored " + std::to_string(ok) + " item(s)");
                break;
            }
            case Op::MoveOutMany: {
                int ok = 0;
                for (std::uint64_t item : cmd.items) {
                    if (stop_.load()) break;
                    if (sam::cs2_gc::move_out(gc, cmd.unit_id, item, err)) ++ok;
                }
                post_snapshot(gc);
                if (cb_.on_action_done)
                    cb_.on_action_done("Removed " + std::to_string(ok) + " item(s)");
                break;
            }
            case Op::ClaimReward: {

                std::vector<std::string> names;
                names.reserve(cmd.items.size());
                for (std::uint64_t id : cmd.items) {
                    const auto it = gc.inventory().find(id);
                    names.push_back(it == gc.inventory().end()
                                        ? std::string()
                                        : schema_->resolve(it->second).name);
                }
                if (redeem_free_reward(gc, cmd.items, err)) {
                    redeemed_gen_time_ = gc.personal_store().generation_time;
                    redeemed_names_ = names;
                    if (cb_.on_reward_claimed)
                        cb_.on_reward_claimed(redeemed_gen_time_, cmd.items, names);
                    if (cb_.on_action_done) cb_.on_action_done("Weekly reward claimed");
                } else if (cb_.on_error) {
                    cb_.on_error(err);
                }
                post_snapshot(gc);
                break;
            }
            case Op::Refresh:
                post_snapshot(gc);
                break;
            case Op::PullProfiles: {

                constexpr int kPaceMs = 1600;
                constexpr int kTailTimeoutMs = 6000;
                const int requested = static_cast<int>(cmd.items.size());
                int received = 0;
                std::unordered_set<std::uint32_t> pending;
                for (std::uint64_t id : cmd.items)
                    pending.insert(static_cast<std::uint32_t>(id));

                auto absorb = [&] {
                    for (ProfileResult& r : gc.take_profiles()) {
                        if (pending.erase(r.account_id) == 0) continue;
                        ++received;
                        if (!cb_.on_profile) continue;
                        PlayerXp xp;
                        xp.valid = true;
                        xp.level = r.level;
                        xp.cur_xp = r.cur_xp;
                        xp.medal_defidx = r.medal_defidx;
                        xp.featured_medal_defidx = r.featured_medal_defidx;
                        ProfilePull pp;
                        pp.account_id = r.account_id;
                        pp.progress = make_progress(xp);
                        pp.featured_medal_defidx = r.featured_medal_defidx;
                        pp.medals = resolve_medals(*schema_, r.medal_defidx);
                        pp.ranks.premier_rating = r.premier_rating;
                        pp.ranks.premier_wins = r.premier_wins;
                        pp.ranks.wingman_rank = r.wingman_rank;
                        pp.ranks.wingman_wins = r.wingman_wins;
                        pp.ranks.cooldown_expires_unix = r.cooldown_expires_unix;
                        pp.ranks.cooldown_reason = r.cooldown_reason;
                        cb_.on_profile(std::move(pp));
                    }
                };

                bool dropped = false;
                for (std::uint64_t raw : cmd.items) {
                    if (stop_.load()) break;
                    gc.request_profile(static_cast<std::uint32_t>(raw));
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(kPaceMs);
                    while (std::chrono::steady_clock::now() < deadline) {
                        if (!gc.pump(150)) { dropped = true; break; }
                        absorb();
                    }
                    if (dropped) break;
                }
                const auto tail =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(kTailTimeoutMs);
                while (!dropped && !pending.empty() && !stop_.load() &&
                       std::chrono::steady_clock::now() < tail) {
                    if (!gc.pump(200)) { dropped = true; break; }
                    absorb();
                }

                if (cb_.on_profiles_done) cb_.on_profiles_done(received, requested);
                break;
            }
        }
    }

    cm.disconnect();
    if (cb_.on_status) cb_.on_status("Disconnected");
}

namespace {

class ClientReaper {
public:
    ~ClientReaper() { drain(); }

    void retire(std::unique_ptr<Cs2GcClient> c) {
        if (c) c->begin_stop();
        std::lock_guard lk(mtx_);
        pending_.push_back(std::move(c));
        if (!worker_.joinable()) worker_ = std::thread([this] { loop(); });
        cv_.notify_one();
    }

    void drain() {
        {
            std::lock_guard lk(mtx_);
            quit_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

private:
    void loop() {
        for (;;) {
            std::unique_ptr<Cs2GcClient> c;
            {
                std::unique_lock lk(mtx_);
                cv_.wait(lk, [this] { return quit_ || !pending_.empty(); });
                if (pending_.empty()) return;
                c = std::move(pending_.front());
                pending_.pop_front();
            }
            c.reset();
        }
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<Cs2GcClient>> pending_;
    std::thread worker_;
    bool quit_ = false;
};

ClientReaper& reaper() {
    static ClientReaper r;
    return r;
}

}  // namespace

void retire(std::unique_ptr<Cs2GcClient> client) { reaper().retire(std::move(client)); }

void reap_all() { reaper().drain(); }

}  // namespace sam::cs2_gc
