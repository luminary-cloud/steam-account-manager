#include "core/account_store/ban_diff.hpp"

#include <atomic>
#include <cstdio>

namespace sam::core {

namespace {

bool is_economy_banned(const std::string& e) {
    return e == "banned" || e == "probation";
}

std::string make_event_id(std::int64_t now) {
    static std::atomic<std::uint64_t> counter{0};
    const auto n = counter.fetch_add(1);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "evt-%016llx%08llx",
                  static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(n));
    return buf;
}

BanEvent make_event(const std::string& account_id, BanEventKind kind,
                    std::int64_t before, std::int64_t after, std::int64_t now) {
    BanEvent ev;
    ev.event_id = make_event_id(now);
    ev.account_id = account_id;
    ev.kind = kind;
    ev.detected_unix = now;
    ev.before = before;
    ev.after = after;
    return ev;
}

}  // namespace

const char* ban_event_kind_label(BanEventKind k) {
    switch (k) {
        case BanEventKind::NewVacBan:       return "VAC ban";
        case BanEventKind::NewGameBan:      return "Game ban";
        case BanEventKind::NewCommunityBan: return "Community ban";
        case BanEventKind::NewTradeBan:     return "Trade ban";
        case BanEventKind::NewVacLive:      return "VAC-Live";
        case BanEventKind::BanRemoved:      return "Ban removed";
        case BanEventKind::CooldownStarted: return "Cooldown started";
        case BanEventKind::CooldownEnded:   return "Cooldown ended";
    }
    return "Change";
}

PreviousSnapshot snapshot_from(const BanStatus& bans, const CS2Status& cs2) {
    PreviousSnapshot s;
    s.vac_ban_count = bans.vac_ban_count;
    s.game_ban_count = bans.game_ban_count;
    s.community_banned = bans.community_banned;
    s.economy_ban = bans.economy_ban;
    s.vac_live = cs2.vac_live;
    s.cooldown_expires_unix = cs2.cooldown_expires_unix;
    return s;
}

std::vector<BanEvent> diff_account_state(const std::string& account_id,
                                         const PreviousSnapshot& prev,
                                         const BanStatus& new_bans,
                                         const CS2Status& new_cs2,
                                         std::int64_t now) {
    std::vector<BanEvent> out;
    if (prev.snapshot_unix == 0) return out;

    if (new_bans.vac_ban_count > prev.vac_ban_count) {
        out.push_back(make_event(account_id, BanEventKind::NewVacBan,
                                  prev.vac_ban_count, new_bans.vac_ban_count, now));
    } else if (new_bans.vac_ban_count < prev.vac_ban_count) {
        out.push_back(make_event(account_id, BanEventKind::BanRemoved,
                                  prev.vac_ban_count, new_bans.vac_ban_count, now));
    }

    if (new_bans.game_ban_count > prev.game_ban_count) {
        out.push_back(make_event(account_id, BanEventKind::NewGameBan,
                                  prev.game_ban_count, new_bans.game_ban_count, now));
    } else if (new_bans.game_ban_count < prev.game_ban_count) {
        out.push_back(make_event(account_id, BanEventKind::BanRemoved,
                                  prev.game_ban_count, new_bans.game_ban_count, now));
    }

    if (new_bans.community_banned && !prev.community_banned) {
        out.push_back(make_event(account_id, BanEventKind::NewCommunityBan, 0, 1, now));
    } else if (!new_bans.community_banned && prev.community_banned) {
        out.push_back(make_event(account_id, BanEventKind::BanRemoved, 1, 0, now));
    }

    const bool prev_trade = is_economy_banned(prev.economy_ban);
    const bool now_trade  = is_economy_banned(new_bans.economy_ban);
    if (now_trade && !prev_trade) {
        out.push_back(make_event(account_id, BanEventKind::NewTradeBan, 0, 1, now));
    } else if (!now_trade && prev_trade) {
        out.push_back(make_event(account_id, BanEventKind::BanRemoved, 1, 0, now));
    }

    // vac_live mirrors bans.vac_banned in the refresh pipeline; only fire it
    // separately when the VAC ban count didn't already account for it.
    const bool vac_count_jumped = new_bans.vac_ban_count > prev.vac_ban_count;
    if (new_cs2.vac_live && !prev.vac_live && !vac_count_jumped) {
        out.push_back(make_event(account_id, BanEventKind::NewVacLive, 0, 1, now));
    }

    // Cooldown transitions. CS2 reports cooldown_expires_unix==0 when no
    // cooldown is active. A non-zero expiry that's in the past means the
    // cooldown ended; switching to zero or to a later expiry counts as well.
    const bool prev_cd = prev.cooldown_expires_unix > 0 &&
                         prev.cooldown_expires_unix > prev.snapshot_unix;
    const bool now_cd  = new_cs2.cooldown_expires_unix > 0 &&
                         new_cs2.cooldown_expires_unix > now;
    if (now_cd && !prev_cd) {
        out.push_back(make_event(account_id, BanEventKind::CooldownStarted,
                                  prev.cooldown_expires_unix,
                                  new_cs2.cooldown_expires_unix, now));
    } else if (!now_cd && prev_cd) {
        out.push_back(make_event(account_id, BanEventKind::CooldownEnded,
                                  prev.cooldown_expires_unix,
                                  new_cs2.cooldown_expires_unix, now));
    }

    return out;
}

}  // namespace sam::core
