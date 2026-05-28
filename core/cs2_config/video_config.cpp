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

}  // namespace sam::cs2_config
