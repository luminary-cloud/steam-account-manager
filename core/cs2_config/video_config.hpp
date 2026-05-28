#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace sam::cs2_config {

struct DeployResult {
    bool ok = false;
    std::string message;            // UTF-8, suitable for a toast or the log
    std::filesystem::path target;
};

// Copies `template_path` into the CS2 (appid 730) per-user config folder for the
// account identified by `steam_id_64`:
//   <Steam>/userdata/<accountid>/730/local/cfg/cs2_video.txt
// Any existing cs2_video.txt is backed up with a UTC timestamp suffix first. The
// cfg folder is created when CS2 has never run for that account on this machine.
DeployResult deploy_video_config(std::uint64_t steam_id_64,
                                 const std::filesystem::path& template_path);

}  // namespace sam::cs2_config
