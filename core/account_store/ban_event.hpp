#pragma once

#include <cstdint>
#include <string>

namespace sam::core {

enum class BanEventKind : std::uint8_t {
    NewVacBan = 0,
    NewGameBan = 1,
    NewCommunityBan = 2,
    NewTradeBan = 3,
    NewVacLive = 4,
    BanRemoved = 5,
    CooldownStarted = 6,
    CooldownEnded = 7,
};

struct BanEvent {
    std::string event_id;
    std::string account_id;
    BanEventKind kind = BanEventKind::NewVacBan;
    std::int64_t detected_unix = 0;
    std::int64_t ack_unix = 0;
    // Meaning depends on kind: ban-count kinds = counts, boolean kinds = 0/1,
    // cooldown kinds = unix timestamps.
    std::int64_t before = 0;
    std::int64_t after = 0;
};

const char* ban_event_kind_label(BanEventKind k);

}  // namespace sam::core
