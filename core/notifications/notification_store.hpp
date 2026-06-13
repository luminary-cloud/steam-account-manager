#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include "core/account_store/ban_event.hpp"

namespace sam::core::notifications {

inline constexpr std::size_t kMaxEvents = 500;

class NotificationStore {
public:
    void set_path(std::filesystem::path path);

    // Silently leaves the store empty on missing or malformed files.
    void load();

    // Append + persist. If the store would exceed kMaxEvents, the oldest acknowledged
    // events are pruned first; oldest unacked if no acked exist.
    void push(BanEvent event);

    std::vector<BanEvent> unacked_for(const std::string& account_id) const;
    std::size_t unacked_count_for(const std::string& account_id) const;
    std::size_t unacked_count() const;

    void acknowledge(const std::string& event_id);
    void acknowledge_all_for(const std::string& account_id);

    // Drops events older than `days` (by detected_unix). Returns the number removed.
    std::size_t prune_older_than(int days, std::int64_t now);

    const std::deque<BanEvent>& events() const { return events_; }

private:
    void save_locked() const;

    std::deque<BanEvent> events_;
    std::filesystem::path path_;
};

}  // namespace sam::core::notifications
