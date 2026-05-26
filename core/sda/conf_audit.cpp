#include "core/sda/conf_audit.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/log.hpp"

namespace sam::sda {

using json = nlohmann::json;

namespace {

std::string make_event_id(std::int64_t now) {
    static std::atomic<std::uint64_t> counter{0};
    const auto n = counter.fetch_add(1);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "aud-%016llx%08llx",
                  static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(n));
    return buf;
}

json to_json(const AuditEntry& e) {
    return json{
        {"event_id",      e.event_id},
        {"unix_time",     e.unix_time},
        {"account_id",    e.account_id},
        {"account_login", e.account_login},
        {"conf_id",       e.conf_id},
        {"conf_type",     static_cast<int>(e.conf_type)},
        {"headline",      e.headline},
        {"allow",         e.allow},
        {"source",        static_cast<int>(e.source)},
    };
}

AuditEntry from_json(const json& j) {
    AuditEntry e;
    e.event_id      = j.value("event_id", std::string{});
    e.unix_time     = j.value("unix_time", static_cast<std::int64_t>(0));
    e.account_id    = j.value("account_id", std::string{});
    e.account_login = j.value("account_login", std::string{});
    e.conf_id       = j.value("conf_id", std::string{});
    e.conf_type     = static_cast<ConfirmationType>(j.value("conf_type", 0));
    e.headline      = j.value("headline", std::string{});
    e.allow         = j.value("allow", true);
    e.source        = static_cast<AuditSource>(j.value("source", 0));
    return e;
}

}  // namespace

void ConfAuditLog::set_path(std::filesystem::path path) { path_ = std::move(path); }

void ConfAuditLog::load() {
    if (path_.empty()) return;
    std::ifstream in(path_);
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("conf_audit: load failed: {}", ex.what());
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

void ConfAuditLog::save_locked() const {
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
        SAM_LOG_WARN("conf_audit: save failed for {}", path_.string());
    }
}

void ConfAuditLog::record(AuditEntry entry) {
    if (entry.event_id.empty()) {
        entry.event_id = make_event_id(entry.unix_time);
    }
    while (entries_.size() >= kMaxAuditEntries) {
        entries_.pop_front();
    }
    entries_.push_back(std::move(entry));
    save_locked();
}

std::size_t ConfAuditLog::prune_older_than(int days, std::int64_t now) {
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

}  // namespace sam::sda
