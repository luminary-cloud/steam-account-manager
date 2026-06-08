#include "core/cs2_config/video_config.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>

#include "core/log.hpp"
#include "platform/registry.hpp"

namespace sam::cs2_config {

namespace {

namespace fs = std::filesystem;

std::wstring timestamp_suffix() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    std::wstringstream ss;
    ss << std::put_time(&tm, L"%Y%m%dT%H%M%SZ");
    return ss.str();
}

// True if `child` is `parent` or lives underneath it. Both should be canonical.
bool is_subpath(const fs::path& child, const fs::path& parent) {
    auto c = child.begin();
    auto p = parent.begin();
    for (; p != parent.end(); ++p, ++c) {
        if (c == child.end() || *c != *p) return false;
    }
    return true;
}

// True if `a` and `b` are the same path or one contains the other. Used to stop a
// recursive copy from feeding on its own freshly-written files (or an import from
// deleting its own source via remove_all). On failure to canonicalize we return
// false and let the copy proceed rather than blocking a valid operation.
bool paths_overlap(const fs::path& a, const fs::path& b) {
    std::error_code ea, eb;
    const fs::path ca = fs::weakly_canonical(a, ea);
    const fs::path cb = fs::weakly_canonical(b, eb);
    if (ea || eb) return false;
    return is_subpath(ca, cb) || is_subpath(cb, ca);
}

struct CopyStats {
    int files_copied = 0;
    int dirs_made = 0;
    int errors = 0;
    std::string first_error;
};

// Recursively copies the CONTENTS of `src` into `dst` (which must already exist),
// overwriting existing files. Symlinks/junctions are skipped so the copy can't
// escape the tree. Per-entry failures are counted and skipped so one locked or
// denied file does not abort the whole copy.
void copy_tree(const fs::path& src, const fs::path& dst, CopyStats& st) {
    std::error_code ec;
    fs::recursive_directory_iterator it(
        src, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    if (ec) {
        ++st.errors;
        if (st.first_error.empty()) st.first_error = ec.message();
        return;
    }

    for (; it != end; it.increment(ec)) {
        if (ec) {
            ++st.errors;
            if (st.first_error.empty()) st.first_error = ec.message();
            break;
        }

        std::error_code item_ec;
        if (it->is_symlink(item_ec)) {
            it.disable_recursion_pending();
            continue;
        }

        const fs::path rel = fs::relative(it->path(), src, item_ec);
        if (item_ec || rel.empty()) {
            ++st.errors;
            if (st.first_error.empty()) st.first_error = "could not resolve relative path";
            continue;
        }
        const fs::path target = dst / rel;

        if (it->is_directory(item_ec)) {
            fs::create_directories(target, item_ec);
            if (item_ec) {
                ++st.errors;
                if (st.first_error.empty()) st.first_error = item_ec.message();
            } else {
                ++st.dirs_made;
            }
            continue;
        }

        if (it->is_regular_file(item_ec)) {
            fs::create_directories(target.parent_path(), item_ec);
            item_ec.clear();
            fs::copy_file(it->path(), target,
                          fs::copy_options::overwrite_existing, item_ec);
            if (item_ec) {
                ++st.errors;
                if (st.first_error.empty()) st.first_error = item_ec.message();
            } else {
                ++st.files_copied;
            }
        }
        // Other entry types (block/char/fifo/socket) are ignored.
    }
}

}  // namespace

DeployResult deploy_video_config(std::uint64_t steam_id_64,
                                 const fs::path& template_path) {
    DeployResult out;

    if (steam_id_64 == 0) {
        out.message = "account has no resolved SteamID";
        return out;
    }

    std::error_code ec;
    if (!fs::exists(template_path, ec)) {
        out.message = "no video config selected";
        return out;
    }

    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return out;
    }

    // userdata folders are named by the SteamID3 account number (lower 32 bits).
    const auto account_id = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    fs::path target = *steam / L"userdata" / std::to_wstring(account_id) /
                      L"730" / L"local" / L"cfg" / L"cs2_video.txt";
    out.target = target;

    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        out.message = "could not create cfg folder: " + ec.message();
        return out;
    }

    if (fs::exists(target, ec)) {
        fs::path bak = target;
        bak += L".bak." + timestamp_suffix();
        fs::copy_file(target, bak, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            out.message = "backup of existing cs2_video.txt failed: " + ec.message();
            return out;
        }
    }

    fs::copy_file(template_path, target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        out.message = "copy failed: " + ec.message();
        return out;
    }

    out.ok = true;
    out.message = "Video config applied";
    SAM_LOG_INFO("cs2 video config: applied to userdata/{}/730/local/cfg/cs2_video.txt",
                 account_id);
    return out;
}

DeployResult deploy_730_folder(std::uint64_t steam_id_64,
                               const fs::path& template_dir) {
    DeployResult out;

    if (steam_id_64 == 0) {
        out.message = "account has no resolved SteamID";
        return out;
    }

    std::error_code ec;
    if (!fs::is_directory(template_dir, ec)) {
        out.message = "no 730 folder selected";
        return out;
    }

    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return out;
    }

    // userdata folders are named by the SteamID3 account number (lower 32 bits).
    const auto account_id = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    fs::path dst_root = *steam / L"userdata" / std::to_wstring(account_id) / L"730";
    out.target = dst_root;

    if (paths_overlap(template_dir, dst_root)) {
        out.message = "source folder overlaps the destination";
        return out;
    }

    fs::create_directories(dst_root, ec);
    if (ec) {
        out.message = "could not create 730 folder: " + ec.message();
        return out;
    }

    CopyStats st;
    copy_tree(template_dir, dst_root, st);

    if (st.files_copied == 0) {
        out.message = st.errors > 0 ? ("730 copy failed: " + st.first_error)
                                    : "730 folder is empty; nothing to copy";
        return out;
    }

    out.ok = true;
    out.message = st.errors == 0
        ? ("CS2 730 folder applied (" + std::to_string(st.files_copied) + " files)")
        : ("CS2 730 folder applied (" + std::to_string(st.files_copied) + " files, " +
           std::to_string(st.errors) + " skipped)");
    SAM_LOG_INFO("cs2 730 folder: copied {} files, {} dirs, {} errors -> userdata/{}/730",
                 st.files_copied, st.dirs_made, st.errors, account_id);
    return out;
}

DeployResult import_730_template(const fs::path& src, const fs::path& dst) {
    DeployResult out;
    out.target = dst;

    std::error_code ec;
    if (!fs::is_directory(src, ec)) {
        out.message = "not a folder";
        return out;
    }

    if (paths_overlap(src, dst)) {
        out.message = "source folder overlaps the snapshot location";
        return out;
    }

    fs::remove_all(dst, ec);  // clear any prior snapshot
    ec.clear();
    fs::create_directories(dst, ec);
    if (ec) {
        out.message = "could not create snapshot folder: " + ec.message();
        return out;
    }

    CopyStats st;
    copy_tree(src, dst, st);

    if (st.files_copied == 0) {
        out.message = st.errors > 0 ? ("import failed: " + st.first_error)
                                    : "folder is empty; nothing imported";
        return out;
    }

    out.ok = true;
    out.message = "Imported " + std::to_string(st.files_copied) + " files";
    SAM_LOG_INFO("cs2 730 template: imported {} files, {} dirs, {} errors",
                 st.files_copied, st.dirs_made, st.errors);
    return out;
}

}  // namespace sam::cs2_config
