#include "core/notifications/notification_store.hpp"

#include <ctime>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/log.hpp"

namespace sam::core::notifications {

using json = nlohmann::json;

namespace {

json to_json(const BanEvent& e) {
    return json{
        {"event_id",      e.event_id},
        {"account_id",    e.account_id},
        {"kind",          static_cast<int>(e.kind)},
        {"detected_unix", e.detected_unix},
        {"ack_unix",      e.ack_unix},
        {"before",        e.before},
        {"after",         e.after},
    };
}

BanEvent from_json(const json& j) {
    BanEvent e;
    e.event_id      = j.value("event_id", std::string{});
    e.account_id    = j.value("account_id", std::string{});
    e.kind          = static_cast<BanEventKind>(j.value("kind", 0));
    e.detected_unix = j.value("detected_unix", static_cast<std::int64_t>(0));
    e.ack_unix      = j.value("ack_unix", static_cast<std::int64_t>(0));
    e.before        = j.value("before", static_cast<std::int64_t>(0));
    e.after         = j.value("after", static_cast<std::int64_t>(0));
    return e;
}

}  // namespace

void NotificationStore::set_path(std::filesystem::path path) {
    path_ = std::move(path);
}

void NotificationStore::load() {
    if (path_.empty()) return;
    std::ifstream in(path_);
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("notifications: load failed: {}", ex.what());
        return;
    }
    events_.clear();
    if (j.contains("events") && j["events"].is_array()) {
        for (const auto& je : j["events"]) {
            try {
                events_.push_back(from_json(je));
            } catch (...) {
            }
        }
    }
}

void NotificationStore::save_locked() const {
    if (path_.empty()) return;
    json j;
    j["events"] = json::array();
    for (const auto& e : events_) {
        j["events"].push_back(to_json(e));
    }
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    std::ofstream out(path_);
    if (out) {
        out << j.dump();
    } else {
        SAM_LOG_WARN("notifications: save failed for {}", path_.string());
    }
}

void NotificationStore::push(BanEvent event) {
    if (events_.size() >= kMaxEvents) {

        auto it = events_.begin();
        for (; it != events_.end(); ++it) {
            if (it->ack_unix > 0) break;
        }
        if (it != events_.end()) {
            events_.erase(it);
        } else {
            events_.pop_front();
        }
    }
    events_.push_back(std::move(event));
    save_locked();
}

std::vector<BanEvent> NotificationStore::unacked_for(const std::string& account_id) const {
    std::vector<BanEvent> out;
    for (const auto& e : events_) {
        if (e.account_id == account_id && e.ack_unix == 0) {
            out.push_back(e);
        }
    }
    return out;
}

std::size_t NotificationStore::unacked_count_for(const std::string& account_id) const {
    std::size_t n = 0;
    for (const auto& e : events_) {
        if (e.account_id == account_id && e.ack_unix == 0) ++n;
    }
    return n;
}

std::size_t NotificationStore::unacked_count() const {
    std::size_t n = 0;
    for (const auto& e : events_) {
        if (e.ack_unix == 0) ++n;
    }
    return n;
}

void NotificationStore::acknowledge(const std::string& event_id) {
    bool changed = false;
    const auto now = std::time(nullptr);
    for (auto& e : events_) {
        if (e.event_id == event_id && e.ack_unix == 0) {
            e.ack_unix = static_cast<std::int64_t>(now);
            changed = true;
            break;
        }
    }
    if (changed) save_locked();
}

void NotificationStore::acknowledge_all_for(const std::string& account_id) {
    bool changed = false;
    const auto now = std::time(nullptr);
    for (auto& e : events_) {
        if (e.account_id == account_id && e.ack_unix == 0) {
            e.ack_unix = static_cast<std::int64_t>(now);
            changed = true;
        }
    }
    if (changed) save_locked();
}

std::size_t NotificationStore::prune_older_than(int days, std::int64_t now) {
    if (days <= 0) return 0;
    const std::int64_t cutoff = now - static_cast<std::int64_t>(days) * 86400;
    const auto before = events_.size();
    while (!events_.empty() && events_.front().detected_unix < cutoff) {
        events_.pop_front();
    }
    const auto removed = before - events_.size();
    if (removed > 0) save_locked();
    return removed;
}

}  // namespace sam::core::notifications
