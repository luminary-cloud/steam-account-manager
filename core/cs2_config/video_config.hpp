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

// Copies the entire `template_dir` tree into the CS2 (appid 730) per-user folder
// for the account identified by `steam_id_64`:
//   <Steam>/userdata/<accountid>/730/
// Files in the template overwrite matching files in the destination; other
// existing files are left untouched. Symlinks are skipped. The 730 folder is
// created when CS2 has never run for that account on this machine.
DeployResult deploy_730_folder(std::uint64_t steam_id_64,
                               const std::filesystem::path& template_dir);

// Snapshots a picked 730 folder into the app's data dir: clears `dst`, then
// recursively copies the contents of `src` into it. Used by Settings so the
// chosen folder survives the original being moved or deleted.
DeployResult import_730_template(const std::filesystem::path& src,
                                 const std::filesystem::path& dst);

}  // namespace sam::cs2_config
