#include "core/steam_local/login_prefs.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "core/log.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/steam_local/vdf_file.hpp"
#include "platform/registry.hpp"

namespace sam::steam_local {

namespace {

namespace fs = std::filesystem;

std::optional<fs::path> account_userdata_dir(std::uint64_t steam_id_64, LoginPrefResult& out) {
    if (steam_id_64 == 0) {
        out.message = "account has no resolved SteamID";
        return std::nullopt;
    }
    auto steam = platform::registry::read_steam_install_dir();
    if (!steam) {
        out.message = "Steam install not found";
        return std::nullopt;
    }

    const auto account_id = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    return *steam / L"userdata" / std::to_wstring(account_id);
}

bool load_for_amend(const fs::path& path, VdfNode& root, LoginPrefResult& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return true;

    const std::string text = read_text_file(path);
    if (text.empty()) {
        out.message = path.filename().string() + " exists but is empty or unreadable; "
                      "left unchanged";
        return false;
    }
    root = parse_vdf(text);
    if (root.children.empty()) {
        out.message = path.filename().string() + " exists but could not be parsed; "
                      "left unchanged";
        return false;
    }
    return true;
}

}  // namespace

LoginPrefResult apply_localconfig_prefs(std::uint64_t steam_id_64,
                                        const LocalConfigPrefs& prefs) {
    LoginPrefResult out;
    if (!prefs.any()) {
        out.ok = true;
        return out;
    }

    auto userdata = account_userdata_dir(steam_id_64, out);
    if (!userdata) return out;

    const fs::path path = *userdata / L"config" / L"localconfig.vdf";
    out.target = path;

    VdfNode root;
    if (!load_for_amend(path, root, out)) return out;

    const auto account_id = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    VdfNode& store = find_or_add_block(root, "UserLocalConfigStore");
    std::string applied;
    auto note = [&applied](const char* what) {
        if (!applied.empty()) applied += ", ";
        applied += what;
    };

    if (prefs.news_notify_off) {
        VdfNode& news = find_or_add_block(store, "news");
        upsert_scalar_ci(news, "NotifyAvailableGames", "0");
        note("news notifications disabled");
    }
    if (prefs.persona_invisible) {

        VdfNode& friends = find_or_add_block(store, "friends");
        upsert_scalar_ci(friends, "SignIntoFriends", "1");
        upsert_scalar_ci(friends, "PersonaStateDesired", "7");

        VdfNode& web_storage = find_or_add_block(store, "WebStorage");
        upsert_scalar_ci(web_storage, "FriendStoreLocalPrefs_" + std::to_string(account_id),
                         R"({"ePersonaState":7,"strNonFriendsAllowedToMsg":""})");
        note("signing in as Invisible");
    }
    if (prefs.remote_play_off) {
        VdfNode& streaming = find_or_add_block(store, "streaming_v2");
        upsert_scalar_ci(streaming, "EnableStreaming", "0");
        note("Remote Play disabled");
    }

    if (!backup_and_write(path, serialize_root(root), true,
                          false, out.message))
        return out;

    out.ok = true;
    out.message = applied;
    SAM_LOG_INFO("login prefs: userdata/{} localconfig -> {}", account_id, applied);
    return out;
}

LoginPrefResult set_cloud_enabled_off(std::uint64_t steam_id_64) {
    LoginPrefResult out;

    auto userdata = account_userdata_dir(steam_id_64, out);
    if (!userdata) return out;

    const fs::path shared = *userdata / L"7" / L"remote" / L"sharedconfig.vdf";
    out.target = shared;

    VdfNode root;
    if (!load_for_amend(shared, root, out)) return out;

    VdfNode& store = find_or_add_block(root, "UserRoamingConfigStore");
    VdfNode& software = find_or_add_block(store, "Software");
    VdfNode& valve = find_or_add_block(software, "Valve");
    VdfNode& steam_blk = find_or_add_block(valve, "Steam");
    upsert_scalar_ci(steam_blk, "CloudEnabled", "0");

    const std::string serialized = serialize_root(root);

    if (!backup_and_write(shared, serialized, false,
                          true, out.message))
        return out;

    out.ok = true;
    out.message = "Steam Cloud disabled";
    SAM_LOG_INFO("login prefs: set CloudEnabled=0 for userdata/{}",
                 static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull));
    return out;
}

LoginConfigPresence login_config_presence(std::uint64_t steam_id_64) {
    LoginConfigPresence out;
    LoginPrefResult ignored;
    auto userdata = account_userdata_dir(steam_id_64, ignored);
    if (!userdata) return out;

    std::error_code ec;
    out.userdata_present = fs::is_directory(*userdata, ec);
    out.localconfig_present =
        fs::is_regular_file(*userdata / L"config" / L"localconfig.vdf", ec);
    out.sharedconfig_present =
        fs::is_regular_file(*userdata / L"7" / L"remote" / L"sharedconfig.vdf", ec);
    return out;
}

}  // namespace sam::steam_local
