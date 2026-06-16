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
// UserRoamingConfigStore > Software > Valve > Steam path if absent, then refreshes the
// matching entry in 7/remotecache.vdf (size/sha1/time + bumped ChangeNumber) so Steam
// treats the local copy as newer and uploads it instead of pulling a stale cloud copy
// that would re-enable cloud. Turns off Steam Cloud for the account. MUST run while
// Steam is shut down. Existing files are backed up first; on a first login the file is
// created (with parent dirs). sharedconfig.vdf is cloud-synced, so on a brand-new account
// Steam may still pull "cloud on" from the server during the first sign-in (no local
// remotecache exists yet to outrank it), in which case it takes effect from the next
// launch. ok=false only if Steam isn't installed or the account has no resolved SteamID;
// the remotecache refresh is best-effort and a missing cache does not fail the call.
LoginPrefResult set_cloud_enabled_off(std::uint64_t steam_id_64);

}  // namespace sam::steam_local
