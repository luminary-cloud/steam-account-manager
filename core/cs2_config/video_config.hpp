#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

// Snapshots the game folders of a picked userdata/<id> folder into the app's data
// dir: clears `dst`, then copies each appid-named (all-digit) subfolder of `src`
// into it. Everything else (config/, ugc/) is deliberately skipped:
// config/localconfig.vdf holds the *target* account's CS2 launch options and login
// prefs, which the launcher writes itself, so a foreign copy must not land on it.
// Every top-level entry left in `dst` is therefore an appid, which is what Settings
// lists back to the user.
DeployResult import_userdata_template(const std::filesystem::path& src,
                                      const std::filesystem::path& dst);

// Copies each appid folder under `template_dir` into <Steam>/userdata/<accountid>/
// for `steam_id_64`. Same merge semantics as deploy_730_folder: template files
// overwrite matching destination files, other existing files are left untouched.
DeployResult deploy_userdata_folder(std::uint64_t steam_id_64,
                                    const std::filesystem::path& template_dir);

}  // namespace sam::cs2_config
