#pragma once

#include <cstdint>
#include <string>

#include "core/cs2_config/video_config.hpp"  // DeployResult

namespace sam::cs2_config {

// Writes `options` into appid 730's LaunchOptions in the launched account's
// localconfig.vdf (<Steam>/userdata/<accountid>/config/localconfig.vdf), creating
// the Software > Valve > Steam > Apps > 730 path if needed. MUST run while Steam is
// shut down for that user, or Steam overwrites the file from memory on exit. The
// existing file is backed up with a UTC timestamp suffix first. Returns ok=false if
// Steam isn't installed or the account has no localconfig.vdf yet (never signed in).
DeployResult apply_launch_options(std::uint64_t steam_id_64, const std::string& options);

}  // namespace sam::cs2_config
