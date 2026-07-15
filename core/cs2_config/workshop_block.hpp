#pragma once

#include <cstdint>

#include "core/cs2_config/video_config.hpp"  // DeployResult

namespace sam::cs2_config {

// Removes the launched account's NOT-yet-installed CS2 workshop subscriptions from the
// machine-wide <Steam>/steamapps/workshop/appworkshop_730.acf and then locks the file
// read-only, so Steam's post-login reconcile can't re-add them and start downloading.
// Only items absent from workshop/content/730 are touched, and only this account's
// SteamID3 is dropped from each item's `subscribedby` list; an item is fully removed
// only when no other account still subscribes to it. Already-installed maps and other
// accounts' entries are left untouched, and the subscription itself stays intact
// server-side (this is a local download block, not an unsubscribe). Download/temp
// staging under workshop/downloads and workshop/temp is left alone.
//
// MUST run while Steam is shut down, or Steam holds/rewrites the acf. The existing acf
// is backed up with a UTC timestamp suffix first. ok=false only if Steam isn't installed
// or the account has no resolved SteamID; a missing acf is treated as success (nothing
// to block). Re-running clears the prior read-only lock before rewriting.
DeployResult apply_workshop_block(std::uint64_t steam_id_64);

// Clears the read-only lock on appworkshop_730.acf so Steam manages CS2 workshop content
// normally again (used when launching a normal account, and as a manual recovery valve).
// Leaves the file contents as-is. No-op if the acf is absent or already writable.
// ok=false only if Steam isn't installed.
DeployResult unlock_workshop_block();

}  // namespace sam::cs2_config
