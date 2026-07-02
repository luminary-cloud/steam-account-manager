#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/account_store/account_meta.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/hwid/hwid_profile.hpp"

namespace sam::core {

enum class TrustLabel : std::uint8_t {
    Unset = 0,
    Green = 1,
    Yellow = 2,
    Red = 3,
};

// What pressing Login on this account does after the Steam client signs in.
enum class LoginMethod : std::uint8_t {
    Normal = 0,
    LaunchCs2 = 1,
    LaunchCs2Gamesense = 2,  // launch CS2 then inject the gamesense loader
    LaunchCs2Luminary  = 3,  // launch CS2 then run the luminary loader (--auto --game=cs2)
};

// Mirror of a Steam Guard maFile. Everything here is sensitive.
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

// Diffed against on the next refresh to surface ban/cooldown changes.
// snapshot_unix == 0 = never observed: the next refresh records it without
// firing events.
struct PreviousSnapshot {
    int vac_ban_count = 0;
    int game_ban_count = 0;
    bool community_banned = false;
    std::string economy_ban;
    bool vac_live = false;
    std::int64_t cooldown_expires_unix = 0;
    std::int64_t snapshot_unix = 0;
};

// cooldown_expires_unix sentinels: 0 = no cooldown, kCooldownNever = permanent
// (non-expiring) cooldown, which Steam renders as "Never" with Level >= 1.
inline constexpr std::int64_t kCooldownNever = INT64_MAX;

// A profile medal/coin/badge. The GC returns only a def_index; the name + icon are
// resolved via the CS2 item schema at pull time and cached here so the detail panel can
// render them (and they survive restarts) without a live GC connection.
struct CS2Medal {
    std::uint32_t def_index = 0;
    std::string name;
    std::string icon_url;
};

// Populated by scraping /gcpd/730.
struct CS2Status {
    int premier_rating  = -1;
    int premier_wins    = -1;
    int wingman_rank    = -1;
    int wingman_wins    = -1;
    int cs2_player_level = -1;             // "CS:GO Profile Rank" on accountmain
    int cs2_player_xp    = -1;             // XP earned towards next CS2 rank
    bool prime_status = false;             // inferred from cs2_player_level + xp
    bool vac_live = false;                 // mirrors BanStatus.vac_banned
    std::int64_t cooldown_expires_unix = 0;  // 0 = none, kCooldownNever = permanent
    std::string cooldown_reason;           // "Griefing", "Untrusted", "Team Damage", "Abandon"
    // Next weekly XP-drop reset (UTC s). "claimed" while now < this; auto-clears
    // once now reaches it. 0 = not claimed.
    std::int64_t weekly_drop_reset_unix = 0;
    // What the weekly drop picked this period, observed via the GC and cached so the
    // claimed names show without a live connection and the picked items can be told
    // apart from un-picked candidates after a reconnect. `generation` pairs with the GC
    // personal store's generation_time (0 = unknown); names are resolved at observe time.
    std::uint32_t weekly_drop_claimed_generation = 0;
    std::vector<std::uint64_t> weekly_drop_claimed_ids;
    std::vector<std::string> weekly_drop_claimed_names;
    std::int64_t last_refreshed_unix = 0;
    // CS2 Game Coordinator pull cache (distinct from the GCPD scrape above). 0 = never
    // pulled; auto-pull skips an account until now - gc_last_pulled_unix exceeds the TTL.
    std::int64_t gc_last_pulled_unix = 0;
    std::uint32_t featured_medal_defidx = 0;  // pinned medal def_index, 0 = none
    std::vector<CS2Medal> medals;             // displayed medals/coins, resolved name + icon
};

// "TotalSpend" from help.steampowered.com/accountdata/AccountSpend.
struct ExternalFundsStatus {
    std::int64_t total_spend_usd_cents = -1;   // -1 = never fetched
    // False = the figure is some other currency (e.g. RMB) and must not be
    // presented as dollars.
    bool currency_is_usd = true;
    std::string currency;                       // raw code as reported, e.g. "USD"
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

    // NFA (Non-Full-Access): authenticates by a JWT refresh token only, no
    // password. refresh_token_expires caches jwt_expiry(refresh_token) so the
    // UI doesn't decode the token every frame.
    bool is_nfa = false;
    std::int64_t refresh_token_expires = 0;

    // SteamClient-audience refresh token for the CS2 Game Coordinator. Password
    // accounts mint this on demand (their mobile-scoped token is rejected by the
    // CM); NFA accounts reuse refresh_token. Empty until set up.
    crypto::SecureString cm_refresh_token;
    std::int64_t cm_refresh_token_expires = 0;

    std::wstring display_name;
    std::wstring notes;
    std::vector<std::string> tag_ids;
    std::string group_id;
    TrustLabel trust = TrustLabel::Unset;
    LoginMethod login_method = LoginMethod::Normal;
    std::optional<std::string> trade_url;
    std::int64_t trade_url_fetched_unix = 0;  // 0 = never fetched; drives the 7-day re-fetch

    // Per-account outbound proxy: scheme://[user:pass@]host:port (socks5, http,
    // https), empty = direct. May embed credentials, hence SecureString.
    crypto::SecureString proxy;

    std::optional<hwid::HwidProfile> hwid;
    bool hwid_excluded = false;

    WebProfile web;
    BanStatus bans;
    CS2Status cs2;
    ExternalFundsStatus funds;

    // Captured each refresh; the next refresh diffs against it to emit BanEvents.
    PreviousSnapshot prev_snapshot;

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
