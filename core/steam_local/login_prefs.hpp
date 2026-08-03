#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace sam::steam_local {

struct LoginPrefResult {
    bool ok = false;
    std::string message;
    std::filesystem::path target;
};

// The per-account settings that live in localconfig.vdf. Applied together in one
// read-modify-write so a launch rewrites (and backs up) the file once instead of per option.
struct LocalConfigPrefs {
    // news > NotifyAvailableGames = "0": Steam's "Notify me about additions or changes to my
    // games, new releases, and upcoming releases" notification.
    bool news_notify_off = false;
    // friends > PersonaStateDesired = "7" (Invisible), with SignIntoFriends = "1" and the
    // matching WebStorage > FriendStoreLocalPrefs_<accountid> blob the friends UI reads back.
    // Writing only the friends block leaves the two disagreeing, so both go together.
    bool persona_invisible = false;
    // streaming_v2 > EnableStreaming = "0": Steam's "Enable Remote Play". Steam only writes
    // this block once the account has run at least once, so it is created when missing.
    bool remote_play_off = false;

    bool any() const { return news_notify_off || persona_invisible || remote_play_off; }
};

// Applies every set option to userdata/<accountid>/config/localconfig.vdf, creating missing
// blocks. MUST run while Steam is shut down, or Steam overwrites the file from memory on exit.
// Existing files are backed up first; one that won't parse is left alone. ok=false only if
// Steam isn't installed, the SteamID is unresolved, or the file couldn't be read/written.
LoginPrefResult apply_localconfig_prefs(std::uint64_t steam_id_64,
                                        const LocalConfigPrefs& prefs);

// Turns Steam Cloud off for the account via userdata/<accountid>/7/remote/sharedconfig.vdf.
// MUST run while Steam is shut down. Existing files are backed up first.
//
// sharedconfig.vdf is itself cloud-synced (app 7), so 7/remotecache.vdf is deliberately left
// untouched: Steam then treats the edit as a pending local change and uploads it on the next
// sync (during the first-login restart) instead of re-downloading the server's "cloud on"
// copy. An amended file is also locked read-only, because Steam's shutdown rewrite would
// otherwise flip CloudEnabled back to 1. A first-login file stays writable so Steam's own
// setup isn't blocked.
LoginPrefResult set_cloud_enabled_off(std::uint64_t steam_id_64);

// Whether the config files backing the settings above already exist. An absent file means the
// next sign-in is a first login, which Steam initializes from scratch and would clobber
// anything pre-written, so the caller must defer the setting until after it. Both-false if the
// SteamID is unresolved or Steam isn't installed, in which case the caller pre-writes normally.
struct LoginConfigPresence {
    bool localconfig_present = false;   // backs apply_localconfig_prefs
    bool sharedconfig_present = false;  // backs set_cloud_enabled_off
    bool userdata_present = false;      // userdata/<accountid> dir exists: Steam set the
                                        // account up locally (used to tell a first-login
                                        // sign-in is far enough along to restart Steam)
};
LoginConfigPresence login_config_presence(std::uint64_t steam_id_64);

}  // namespace sam::steam_local
