#include "core/trade/trade_audit.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/log.hpp"

namespace sam::core::trade {

using json = nlohmann::json;

namespace {

std::string make_event_id(std::int64_t now) {
    static std::atomic<std::uint64_t> counter{0};
    const auto n = counter.fetch_add(1);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "trd-%016llx%08llx",
                  static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(n));
    return buf;
}

json to_json(const TradeAuditEntry& e) {
    return json{
        {"event_id",      e.event_id},
        {"unix_time",     e.unix_time},
        {"account_id",    e.account_id},
        {"account_login", e.account_login},
        {"offer_id",      e.offer_id},
        {"item_count",    e.item_count},
        {"outcome",       static_cast<int>(e.outcome)},
        {"detail",        e.detail},
        {"source",        static_cast<int>(e.source)},
    };
}

TradeAuditEntry from_json(const json& j) {
    TradeAuditEntry e;
    e.event_id      = j.value("event_id", std::string{});
    e.unix_time     = j.value("unix_time", static_cast<std::int64_t>(0));
    e.account_id    = j.value("account_id", std::string{});
    e.account_login = j.value("account_login", std::string{});
    e.offer_id      = j.value("offer_id", std::string{});
    e.item_count    = j.value("item_count", 0);
    e.outcome       = static_cast<TradeAuditOutcome>(j.value("outcome", 0));
    e.detail        = j.value("detail", std::string{});
    e.source        = static_cast<TradeAuditSource>(j.value("source", 0));
    return e;
}

}  // namespace

void TradeAuditLog::set_path(std::filesystem::path path) { path_ = std::move(path); }

void TradeAuditLog::load() {
    if (path_.empty()) return;
    std::ifstream in(path_);
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("trade_audit: load failed: {}", ex.what());
        return;
    }
    entries_.clear();
    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& je : j["entries"]) {
            try {
                entries_.push_back(from_json(je));
            } catch (...) {
            }
        }
    }
}

void TradeAuditLog::save_locked() const {
    if (path_.empty()) return;
    json j;
    j["entries"] = json::array();
    for (const auto& e : entries_) {
        j["entries"].push_back(to_json(e));
    }
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    std::ofstream out(path_);
    if (out) {
        out << j.dump();
    } else {
        SAM_LOG_WARN("trade_audit: save failed for {}", path_.string());
    }
}

void TradeAuditLog::record(TradeAuditEntry entry) {
    if (entry.event_id.empty()) {
        entry.event_id = make_event_id(entry.unix_time);
    }
    while (entries_.size() >= kMaxTradeAuditEntries) {
        entries_.pop_front();
    }
    entries_.push_back(std::move(entry));
    save_locked();
}

std::size_t TradeAuditLog::prune_older_than(int days, std::int64_t now) {
    if (days <= 0) return 0;
    const std::int64_t cutoff = now - static_cast<std::int64_t>(days) * 86400;
    const auto before = entries_.size();
    while (!entries_.empty() && entries_.front().unix_time < cutoff) {
        entries_.pop_front();
    }
    const auto removed = before - entries_.size();
    if (removed > 0) save_locked();
    return removed;
}

}  // namespace sam::core::trade
