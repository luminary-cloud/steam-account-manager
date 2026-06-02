#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/account_store/group.hpp"
#include "core/account_store/tag.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::core {

enum class TrustLabel : std::uint8_t {
    Unset = 0,
    Green = 1,
    Yellow = 2,
    Red = 3,
};

// Mirror of a Steam Guard mobile authenticator maFile. Everything here is sensitive.
struct SteamGuardAccount {
    std::string shared_secret;     // base64
    std::string serial_number;
    std::string revocation_code;   // "R12345"
    std::string uri;               // otpauth://...
    std::int64_t server_time = 0;
    std::string account_name;
    std::string token_gid;
    std::string identity_secret;   // base64
    std::string secret_1;          // base64
    int status = 1;
    std::string device_id;         // "android:<uuid>"
    bool fully_enrolled = true;
};

// Cached output of the Steam Web API.
struct WebProfile {
    std::string persona_name;
    std::string profile_url;
    std::string avatar_url_full;
    std::string country_code;
    std::int64_t created_unix = 0;
    int community_visibility_state = 0;
    int profile_state = 0;
    int steam_level = -1;
    int owned_games_count = -1;
    std::int64_t total_playtime_minutes = 0;
    std::int64_t last_refreshed_unix = 0;
};

struct BanStatus {
    bool community_banned = false;
    bool vac_banned = false;
    int vac_ban_count = 0;
    int game_ban_count = 0;
    int days_since_last_ban = 0;
    std::string economy_ban;       // "none", "probation", "banned"
    std::int64_t last_refreshed_unix = 0;
};

// Previous values we diff against on the next refresh to surface ban or
// cooldown changes as notifications. snapshot_unix == 0 means "never observed",
// in which case the next refresh records the snapshot without firing events.
struct PreviousSnapshot {
    int vac_ban_count = 0;
    int game_ban_count = 0;
    bool community_banned = false;
    std::string economy_ban;
    bool vac_live = false;
    std::int64_t cooldown_expires_unix = 0;
    std::int64_t snapshot_unix = 0;
};

// CS2-specific fields. All populated by scraping the /gcpd/730 page.
// CS2 has no Danger Zone, so we track Premier and Wingman.
struct CS2Status {
    int premier_rating  = -1;
    int premier_wins    = -1;
    int wingman_rank    = -1;
    int wingman_wins    = -1;
    int cs2_player_level = -1;             // "CS:GO Profile Rank" on accountmain
    int cs2_player_xp    = -1;             // XP earned towards next CS2 rank
    bool prime_status = false;             // inferred from cs2_player_level + xp
    bool vac_live = false;                 // mirrors BanStatus.vac_banned
    std::int64_t cooldown_expires_unix = 0;
    std::string cooldown_reason;           // "Griefing", "Untrusted", "Team Damage", "Abandon"
    std::int64_t last_refreshed_unix = 0;
};

struct Account {
    std::string id;                       // ULID, stable
    std::uint64_t steam_id_64 = 0;        // 0 until resolved
    std::string login;
    crypto::SecureString password;
    std::optional<SteamGuardAccount> sda;

    // Authenticated session for /mobileconf and confirmations.
    crypto::SecureString access_token;
    crypto::SecureString refresh_token;
    std::int64_t access_token_expires = 0;
    std::string session_id;
    crypto::SecureString steam_login_secure;

    // NFA (Non-Full-Access): authenticates by a JWT refresh token only, with no
    // password. Set by the JWT-token import path. refresh_token_expires caches
    // jwt_expiry(refresh_token) so the UI doesn't decode the token every frame.
    bool is_nfa = false;
    std::int64_t refresh_token_expires = 0;

    // User-curated.
    std::wstring display_name;
    std::wstring notes;
    std::vector<std::string> tag_ids;
    std::string group_id;
    TrustLabel trust = TrustLabel::Unset;
    std::optional<std::string> trade_url;

    // Cached info.
    WebProfile web;
    BanStatus bans;
    CS2Status cs2;

    // Captured at the end of each refresh; the next refresh diffs against it
    // to emit BanEvents.
    PreviousSnapshot prev_snapshot;

    // Bookkeeping.
    std::int64_t created_unix = 0;
    std::int64_t last_login_unix = 0;
};

struct Vault {
    int schema_version = 1;
    std::vector<Tag> tags;
    std::vector<Group> groups;
    std::vector<Account> accounts;
};

}  // namespace sam::core
