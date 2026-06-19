#include "core/cs2_gc/casket_ops.hpp"

#include <algorithm>
#include <chrono>

#include "core/cs2_gc/gc_emsg.hpp"
#include "core/cs2_gc/gc_session.hpp"
#include "core/log.hpp"
#include "core/steam_auth/gen/econ_gcmessages.pb.h"

namespace sam::cs2_gc {

namespace {

using clock = std::chrono::steady_clock;

bool contains(const std::vector<std::uint64_t>& v, std::uint64_t id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

void drain_notifications(GcSession& gc) {
    GcNotification n;
    while (gc.pop_notification(n)) {
    }
}

int count_in_unit(GcSession& gc, std::uint64_t unit_id) {
    int n = 0;
    for (const auto& [id, item] : gc.inventory())
        if (item.casket_id == unit_id) ++n;
    return n;
}

}  // namespace

bool load_unit(GcSession& gc, std::uint64_t unit_id, std::vector<EconItem>& out,
               std::string& error) {
    drain_notifications(gc);

    CMsgCasketItem req;
    req.set_casket_item_id(unit_id);
    req.set_item_item_id(unit_id);
    if (!gc.send_gc(GcMsg::CasketItemLoadContents, req)) {
        error = "failed to request storage unit contents";
        return false;
    }

    int expected = -1;
    if (const auto it = gc.inventory().find(unit_id);
        it != gc.inventory().end() && it->second.is_storage_unit)
        expected = it->second.casket_item_count;

    const auto deadline = clock::now() + std::chrono::seconds(20);
    bool contents_notified = false;
    while (clock::now() < deadline) {
        GcNotification n;
        while (gc.pop_notification(n))
            if (n.request == NotifyCasketContents) contents_notified = true;

        if (expected >= 0 && count_in_unit(gc, unit_id) >= expected) break;
        if (expected < 0 && contents_notified) break;
        if (!gc.pump(500)) {
            error = "connection dropped while loading contents";
            return false;
        }
    }

    out.clear();
    for (const auto& [id, item] : gc.inventory())
        if (item.casket_id == unit_id) out.push_back(item);
    SAM_LOG_INFO("gc: storage unit {} loaded {} items", unit_id, out.size());
    return true;
}

bool move_in(GcSession& gc, std::uint64_t unit_id, std::uint64_t item_id, std::string& error) {
    drain_notifications(gc);

    CMsgCasketItem req;
    req.set_casket_item_id(unit_id);
    req.set_item_item_id(item_id);
    if (!gc.send_gc(GcMsg::CasketItemAdd, req)) {
        error = "failed to send move-in request";
        return false;
    }

    const auto deadline = clock::now() + std::chrono::seconds(15);
    while (clock::now() < deadline) {
        GcNotification n;
        while (gc.pop_notification(n)) {
            if (n.request == NotifyCasketTooFull) {
                error = "the storage unit is full";
                return false;
            }
            if (n.request == NotifyCasketAdded && contains(n.item_ids, item_id)) return true;
        }
        const auto it = gc.inventory().find(item_id);
        if (it == gc.inventory().end() || it->second.casket_id != 0) return true;
        if (!gc.pump(500)) {
            error = "connection dropped during move-in";
            return false;
        }
    }
    error = "timed out waiting for the item to move in";
    return false;
}

bool move_out(GcSession& gc, std::uint64_t unit_id, std::uint64_t item_id, std::string& error) {
    drain_notifications(gc);

    CMsgCasketItem req;
    req.set_casket_item_id(unit_id);
    req.set_item_item_id(item_id);
    if (!gc.send_gc(GcMsg::CasketItemExtract, req)) {
        error = "failed to send move-out request";
        return false;
    }

    const auto deadline = clock::now() + std::chrono::seconds(15);
    while (clock::now() < deadline) {
        GcNotification n;
        while (gc.pop_notification(n)) {
            if (n.request == NotifyCasketInvFull) {
                error = "your inventory is full";
                return false;
            }
            if (n.request == NotifyCasketRemoved && contains(n.item_ids, item_id)) return true;
        }
        const auto it = gc.inventory().find(item_id);
        if (it != gc.inventory().end() && it->second.casket_id == 0) return true;
        if (!gc.pump(500)) {
            error = "connection dropped during move-out";
            return false;
        }
    }
    error = "timed out waiting for the item to move out";
    return false;
}

}  // namespace sam::cs2_gc
