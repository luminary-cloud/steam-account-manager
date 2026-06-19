#include "core/cs2_gc/free_reward.hpp"

#include <algorithm>
#include <chrono>

#include "core/cs2_gc/gc_emsg.hpp"
#include "core/cs2_gc/gc_session.hpp"
#include "core/log.hpp"
#include "core/steam_auth/gen/cstrike15_gcmessages.pb.h"

namespace sam::cs2_gc {

bool redeem_free_reward(GcSession& gc, const std::vector<std::uint64_t>& item_ids,
                        std::string& error) {
    const PersonalStore& store = gc.personal_store();
    if (!store.loaded) {
        error = "the weekly reward has not loaded yet";
        return false;
    }
    if (item_ids.empty()) {
        error = "no reward selected";
        return false;
    }
    if (item_ids.size() > store.redeemable_balance) {
        error = "you can only claim " + std::to_string(store.redeemable_balance) + " item(s)";
        return false;
    }
    for (const std::uint64_t id : item_ids)
        if (std::find(store.items.begin(), store.items.end(), id) == store.items.end()) {
            error = "that reward is not available to claim";
            return false;
        }

    GcNotification stale;
    while (gc.pop_notification(stale)) {
    }

    CMsgGCCstrike15_v2_ClientRedeemFreeReward req;
    req.set_generation_time(store.generation_time);
    req.set_redeemable_balance(store.redeemable_balance);
    for (const std::uint64_t id : item_ids) req.add_items(id);
    if (!gc.send_gc(GcMsg::ClientRedeemFreeReward, req)) {
        error = "failed to send reward claim";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        GcNotification n;
        while (gc.pop_notification(n))
            if (n.request == NotifyRedeemFreeReward) {
                SAM_LOG_INFO("gc: weekly reward claimed ({} item(s))", item_ids.size());
                return true;
            }
        if (!gc.pump(500)) {
            error = "connection dropped during reward claim";
            return false;
        }
    }
    error = "timed out waiting for the reward claim";
    return false;
}

}  // namespace sam::cs2_gc
