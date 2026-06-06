#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>

namespace sam::core::trade {

inline constexpr std::size_t kMaxTradeAuditEntries = 1000;

enum class TradeAuditOutcome : std::uint8_t {
    Sent = 0,               // offer sent, no confirmation needed
    SentAndConfirmed = 1,   // offer sent + auto mobile-confirmed
    NeedsConfirmation = 2,  // offer sent, awaiting mobile confirmation
    Failed = 3,             // send failed
};

enum class TradeAuditSource : std::uint8_t {
    UserSingle = 0,
    UserBulk = 1,
};

struct TradeAuditEntry {
    std::string event_id;
    std::int64_t unix_time = 0;
    std::string account_id;
    std::string account_login;
    std::string offer_id;  // tradeofferid when known, empty on failure
    int item_count = 0;    // number of items offered
    TradeAuditOutcome outcome = TradeAuditOutcome::Sent;
    std::string detail;  // error message or confirmation note
    TradeAuditSource source = TradeAuditSource::UserSingle;
};

class TradeAuditLog {
public:
    void set_path(std::filesystem::path path);
    void load();

    void record(TradeAuditEntry entry);

    const std::deque<TradeAuditEntry>& entries() const { return entries_; }

    std::size_t prune_older_than(int days, std::int64_t now);

private:
    void save_locked() const;

    std::deque<TradeAuditEntry> entries_;
    std::filesystem::path path_;
};

}  // namespace sam::core::trade
