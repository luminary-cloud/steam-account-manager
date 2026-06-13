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

// Copies `template_path` to <Steam>/userdata/<accountid>/730/local/cfg/cs2_video.txt
// for `steam_id_64`. Any existing cs2_video.txt is backed up with a UTC timestamp
// suffix first.
DeployResult deploy_video_config(std::uint64_t steam_id_64,
                                 const std::filesystem::path& template_path);

// Copies the `template_dir` tree into <Steam>/userdata/<accountid>/730/ for
// `steam_id_64`. Template files overwrite matching destination files; other existing
// files are left untouched. Symlinks are skipped.
DeployResult deploy_730_folder(std::uint64_t steam_id_64,
                               const std::filesystem::path& template_dir);

// Snapshots a picked 730 folder into the app's data dir: clears `dst`, then
// recursively copies the contents of `src` into it. Used by Settings so the
// chosen folder survives the original being moved or deleted.
DeployResult import_730_template(const std::filesystem::path& src,
                                 const std::filesystem::path& dst);

}  // namespace sam::cs2_config
