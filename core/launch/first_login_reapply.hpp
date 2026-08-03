#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/account_store/account.hpp"
#include "core/hwid/hwid_profile.hpp"
#include "core/launch/steam_launcher.hpp"     // LaunchOptions
#include "core/steam_local/login_prefs.hpp"   // LoginConfigPresence

namespace sam::launch::first_login_reapply {

// The per-account settings a first login would wipe, so they have to be re-applied after it.
struct DeferredPrefs {
    bool launch_options = false;
    bool news = false;
    bool cloud = false;
    bool persona = false;
    bool remote_play = false;

    bool any() const {
        return launch_options || news || cloud || persona || remote_play;
    }
};

// A first login (no per-user config yet) is initialized from scratch by Steam, which
// overwrites anything the launcher pre-wrote while Steam was down. Those settings are
// deferred instead: once the sign-in has finished setting the account up, Steam is shut
// back down, the settings are applied in-place, and Steam is relaunched with auto-login.
// One restart, first login only. Shared by both sign-in methods (the login driver fires it
// from on_login_confirmed, the token launcher from its own worker).
struct Params {
    std::filesystem::path steam_exe;
    std::uint64_t steam_id_64 = 0;
    std::string login_lower;              // lowercased Steam account name

    std::string cs2_launch_options;       // read only when deferred.launch_options
    DeferredPrefs deferred;

    // 0: the caller already confirmed the sign-in (the login driver watches the login
    // window). Non-zero: poll the registry's ActiveUser for this many seconds first and
    // abandon the whole reapply on timeout - Steam is then most likely still sitting on a
    // login window (rejected token), and shutting it down would be worse than losing the
    // settings until the next launch.
    int sign_in_timeout_seconds = 0;
    // Clear AlwaysShowUserChooser before the relaunch, so the account picker can't stall
    // the auto-login. Only the token path needs it (it already does this pre-launch).
    bool clear_account_chooser = false;
    // The raw ConnectCache value the launcher injected, if it injected one (token path).
    // Steam consumes and rotates that token during the sign-in, so it must not be the value
    // restored across the restart; set it here and the reapply waits for Steam's replacement
    // instead. Left empty by the driver path, where anything in the ConnectCache is Steam's
    // own fresh remember-me token.
    std::optional<std::string> injected_connect_cache;

    // Resumed after the relaunch: the caller skips its own autostart for a deferred launch
    // so CS2 isn't racing the restart. Loader paths are per cs2_autostart::start_async.
    core::LoginMethod method = core::LoginMethod::Normal;
    std::filesystem::path gamesense_loader;
    std::filesystem::path luminary_loader;

    std::optional<core::hwid::HwidProfile> hwid;  // re-injected into the relaunched steam.exe
    std::uint32_t hwid_mask = 0x3FFu;
};

// Writes every per-account setting in `opts` that the upcoming sign-in will leave alone, and
// returns the ones a first login would wipe for the caller to defer into Params. Steam must
// already be shut down. `log_tag` prefixes the warnings so the two sign-in paths stay
// distinguishable in the log.
DeferredPrefs apply_login_prefs(std::uint64_t steam_id_64, const LaunchOptions& opts,
                                const steam_local::LoginConfigPresence& presence,
                                std::string_view log_tag);

// Blocks; call from a detached worker, never the UI thread. Must not touch UI/vault state.
// `still_current` returns false once a newer launch supersedes this one, at which point the
// reapply bails out (before the relaunch, so it can't sign the wrong account back in).
void run(const Params& p, const std::function<bool()>& still_current);

// True from the moment a reapply is scheduled until the relaunch's sign-in has rotated the
// stored login token (or the reapply gave up). The token read-back watchers wait this out
// before adopting Steam's ConnectCache token: read mid-restart, they'd store a token that
// the relaunch's second sign-in immediately supersedes - a stale token drops the next launch
// to the login form.
bool in_flight();

// mark_scheduled() is called synchronously by the launcher, before spawning the worker, so a
// watcher started right after the launch can never miss the reapply it has to wait for;
// clear_scheduled() must then be called exactly once, after run() returns (including when it
// bailed out or threw). Only the token path needs this: the driver path is never used for an
// account whose token is read back.
void mark_scheduled();
void clear_scheduled();

}  // namespace sam::launch::first_login_reapply
