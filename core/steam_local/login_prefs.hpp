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

// Sets news > NotifyAvailableGames = "0" in the launched account's localconfig.vdf
// (<Steam>/userdata/<accountid>/config/localconfig.vdf), creating the news block if
// absent. Turns off Steam's "Notify me about additions or changes to my games, new
// releases, and upcoming releases" notification. MUST run while Steam is shut down for
// that user, or Steam overwrites the file from memory on exit. The existing file is
// backed up with a UTC timestamp suffix first. On a first login the file doesn't exist
// yet, so it (and its parent dirs) are created with just this setting. ok=false only if
// Steam isn't installed or the account has no resolved SteamID.
LoginPrefResult set_news_notify_off(std::uint64_t steam_id_64);

// Sets CloudEnabled = "0" in the launched account's sharedconfig.vdf
// (<Steam>/userdata/<accountid>/7/remote/sharedconfig.vdf), creating the
// UserRoamingConfigStore > Software > Valve > Steam path if absent. Turns off Steam Cloud
// for the account. MUST run while Steam is shut down. Existing files are backed up first;
// on a first login the file is created (with parent dirs). sharedconfig.vdf is itself a
// cloud-synced file (app 7), so to make the change stick for an account that already has
// cloud data we deliberately leave 7/remotecache.vdf UNTOUCHED: that makes Steam treat the
// edit as an ordinary pending local change and upload it (disabling Cloud server-side) on
// the next sync, rather than re-downloading the server's "cloud on" copy. The upload lands
// on the first-login restart's relaunch. ok=false only if Steam isn't installed or the
// account has no resolved SteamID.
LoginPrefResult set_cloud_enabled_off(std::uint64_t steam_id_64);

// Whether the per-user config files backing the news / cloud settings already exist on
// disk for the account. An absent file means the upcoming sign-in is a first login that
// Steam will initialize from scratch, clobbering anything pre-written, so the setting has
// to be (re)applied after that first login instead. Returns both-false if the SteamID is
// unresolved or Steam isn't installed, in which case the caller falls back to the normal
// pre-write (no deferral).
struct LoginConfigPresence {
    bool localconfig_present = false;   // backs set_news_notify_off
    bool sharedconfig_present = false;  // backs set_cloud_enabled_off
    bool userdata_present = false;      // userdata/<accountid> dir exists: Steam set the
                                        // account up locally (used to tell a first-login
                                        // sign-in is far enough along to restart Steam)
};
LoginConfigPresence login_config_presence(std::uint64_t steam_id_64);

}  // namespace sam::steam_local
