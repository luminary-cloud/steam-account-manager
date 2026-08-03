#pragma once

#include <cstdint>

#include "core/cs2_config/video_config.hpp"  // DeployResult

namespace sam::cs2_config {

// Sets `disabled_locally` on every CS2 workshop subscription in the account's own
// userdata/<id32>/ugc/730_subscriptions.vdf. That is Steam's "subscribed but don't fetch or
// mount here" state, so nothing is unsubscribed server-side. The file is backed up first.
//
// MUST run while Steam is shut down, or Steam rewrites the file from memory on exit. ok=false
// only if the SteamID is unresolved, Steam isn't installed, or the write fails; a missing
// subscription list is success.
DeployResult apply_workshop_block(std::uint64_t steam_id_64);

// Clears the block for one account, so Steam downloads and mounts its subscribed CS2
// workshop items normally again. Same file and same constraints as apply_workshop_block.
DeployResult clear_workshop_block(std::uint64_t steam_id_64);

// Recovery valve: clears the block for every account with a CS2 subscription list under
// <Steam>/userdata. ok=false only if Steam isn't installed.
DeployResult clear_all_workshop_blocks();

}  // namespace sam::cs2_config
