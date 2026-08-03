#include "core/cleaner/profiles.hpp"

namespace sam::cleaner {
namespace {

std::vector<std::string> caches_and_logs() {
    return {"steam.dumps",
            "steam.logs",
            "steam.appcache",
            "steam.appcache_stats",
            "steam.depotcache",
            "steam.shadercache",
            "steam.workshop_temp",
            "steam.steamapps_downloading",
            "steam.avatarcache",
            "steam.tenfoot_httpcache",
            "steam.overlayhtmlcache",
            "crash.local_appdata"};
}

std::vector<std::string> account_residue() {
    auto ids = caches_and_logs();
    for (const char* id : {"steam.htmlcache", "steam.config_vdf", "steam.local_vdf",
                           "steam.loginusers", "steam.ssfn", "steam.reg.autologin",
                           "steam.reg.users", "steam.reg.activeprocess", "steam.reg.apps",
                           "steam.reg.url_handler", "steam.remoteclients",
                           "steam.userdata_inventory_cache", "steam.userdata_librarycache",
                           "steam.userdata_sharedconfig", "steam.userdata_ugcmsgcache"}) {
        ids.emplace_back(id);
    }
    return ids;
}

std::vector<std::string> full_wipe() {
    auto ids = account_residue();
    for (const char* id : {"steam.userdata", "steam.userdata_shortcuts",
                           "steam.userdata_screenshots", "steam.controller_configs",
                           "steam.userdata_gamerecordings"}) {
        ids.emplace_back(id);
    }
    return ids;
}

const std::vector<Profile>& builtins() {
    static const std::vector<Profile> profiles = [] {
        std::vector<Profile> p;
        p.push_back(Profile{L"Quick Clean",
                            L"Caches, logs and crash dumps only. No account is signed out and "
                            L"nothing account-specific is touched, so this is safe to run on "
                            L"every launch.",
                            caches_and_logs(), false});
        p.push_back(Profile{L"Account Reset",
                            L"Quick Clean plus the account residue: loginusers.vdf, the "
                            L"ConnectCache login tokens in config.vdf and local.vdf, ssfn "
                            L"sentry files, the HKCU autologin values, htmlcache and the "
                            L"per-account caches. loginusers.vdf and the ConnectCache are edited "
                            L"entry by entry, so accounts on the keep list stay signed in; every "
                            L"other account is signed out of this PC.",
                            account_residue(), false});
        p.push_back(Profile{L"Full Wipe",
                            L"Account Reset plus each non-preserved account's whole userdata "
                            L"folder: cloud saves, game settings, CS2 configs and launch "
                            L"options, controller bindings, screenshots and non-Steam shortcuts. "
                            L"Destructive and not undoable.",
                            full_wipe(), true});
        return p;
    }();
    return profiles;
}

}  // namespace

std::span<const Profile> built_in_profiles() {
    const auto& p = builtins();
    return {p.data(), p.size()};
}

}  // namespace sam::cleaner
