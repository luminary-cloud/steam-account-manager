#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>

#include "core/sda/confirmation.hpp"

namespace sam::sda {

inline constexpr std::size_t kMaxAuditEntries = 1000;

enum class AuditSource : std::uint8_t {
    UserSingle = 0,
    UserBulk = 1,
    Auto = 2,
};

struct AuditEntry {
    std::string event_id;
    std::int64_t unix_time = 0;
    std::string account_id;
    std::string account_login;
    std::string conf_id;
    ConfirmationType conf_type = ConfirmationType::Unknown;
    std::string headline;
    bool allow = true;
    AuditSource source = AuditSource::UserSingle;
};

class ConfAuditLog {
public:
    void set_path(std::filesystem::path path);
    void load();

    void record(AuditEntry entry);

    const std::deque<AuditEntry>& entries() const { return entries_; }

    std::size_t prune_older_than(int days, std::int64_t now);

private:
    void save_locked() const;

    std::deque<AuditEntry> entries_;
    std::filesystem::path path_;
};

}  // namespace sam::sda
