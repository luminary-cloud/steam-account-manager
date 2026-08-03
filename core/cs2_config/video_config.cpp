#include "core/cs2_config/video_config.hpp"

#include <algorithm>
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

bool is_subpath(const fs::path& child, const fs::path& parent) {
    auto c = child.begin();
    auto p = parent.begin();
    for (; p != parent.end(); ++p, ++c) {
        if (c == child.end() || *c != *p) return false;
    }
    return true;
}

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

    }
}

bool is_appid_folder(const std::wstring& name) {
    if (name.empty()) return false;
    return std::all_of(name.begin(), name.end(),
                       [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

void copy_appid_folders(const fs::path& src, const fs::path& dst, CopyStats& st,
                        std::vector<std::wstring>& appids_out) {
    std::error_code ec;
    fs::directory_iterator it(src, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
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
        if (it->is_symlink(item_ec) || !it->is_directory(item_ec)) continue;

        const std::wstring name = it->path().filename().wstring();
        if (!is_appid_folder(name)) continue;

        const fs::path target = dst / name;
        fs::create_directories(target, item_ec);
        if (item_ec) {
            ++st.errors;
            if (st.first_error.empty()) st.first_error = item_ec.message();
            continue;
        }
        ++st.dirs_made;

        const int before = st.files_copied;
        copy_tree(it->path(), target, st);
        if (st.files_copied > before) appids_out.push_back(name);
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

    fs::remove_all(dst, ec);
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

DeployResult import_userdata_template(const fs::path& src, const fs::path& dst) {
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

    fs::remove_all(dst, ec);
    ec.clear();
    fs::create_directories(dst, ec);
    if (ec) {
        out.message = "could not create snapshot folder: " + ec.message();
        return out;
    }

    std::vector<std::wstring> appids;
    CopyStats st;
    copy_appid_folders(src, dst, st, appids);

    if (st.files_copied == 0) {
        out.message = st.errors > 0
            ? ("import failed: " + st.first_error)
            : "no game folders found in that userdata folder";
        return out;
    }

    out.ok = true;
    out.message = "Imported " + std::to_string(st.files_copied) + " files across " +
                  std::to_string(appids.size()) + " games";
    SAM_LOG_INFO("userdata template: imported {} files across {} games, {} errors",
                 st.files_copied, appids.size(), st.errors);
    return out;
}

DeployResult deploy_userdata_folder(std::uint64_t steam_id_64,
                                    const fs::path& template_dir) {
    DeployResult out;

    if (steam_id_64 == 0) {
        out.message = "account has no resolved SteamID";
        return out;
    }

    std::error_code ec;
    if (!fs::is_directory(template_dir, ec)) {
        out.message = "no userdata folder selected";
        return out;
    }

    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return out;
    }

    const auto account_id = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    fs::path dst_root = *steam / L"userdata" / std::to_wstring(account_id);
    out.target = dst_root;

    if (paths_overlap(template_dir, dst_root)) {
        out.message = "source folder overlaps the destination";
        return out;
    }

    fs::create_directories(dst_root, ec);
    if (ec) {
        out.message = "could not create userdata folder: " + ec.message();
        return out;
    }

    std::vector<std::wstring> appids;
    CopyStats st;
    copy_appid_folders(template_dir, dst_root, st, appids);

    if (st.files_copied == 0) {
        out.message = st.errors > 0 ? ("userdata copy failed: " + st.first_error)
                                    : "no game folders to copy";
        return out;
    }

    out.ok = true;
    out.message = "Game folders applied (" + std::to_string(st.files_copied) +
                  " files across " + std::to_string(appids.size()) + " games)";
    if (st.errors > 0) {
        out.message += " (" + std::to_string(st.errors) + " skipped)";
    }
    SAM_LOG_INFO("userdata folders: copied {} files, {} games, {} errors -> userdata/{}",
                 st.files_copied, appids.size(), st.errors, account_id);
    return out;
}

}  // namespace sam::cs2_config
