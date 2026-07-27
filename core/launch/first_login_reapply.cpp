#include "core/launch/first_login_reapply.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include "core/cs2_config/launch_options.hpp"
#include "core/launch/cs2_autostart.hpp"
#include "core/launch/hwid_inject.hpp"
#include "core/launch/steam_launcher.hpp"
#include "core/log.hpp"
#include "core/steam_local/connect_cache.hpp"
#include "core/steam_local/login_prefs.hpp"
#include "core/steam_local/loginusers.hpp"
#include "platform/process.hpp"
#include "platform/registry.hpp"

namespace sam::launch::first_login_reapply {

namespace {

constexpr auto kSetupPollInterval = std::chrono::milliseconds(250);
constexpr auto kMinSetupDwell     = std::chrono::seconds(5);
constexpr auto kSetupTimeout      = std::chrono::seconds(60);
constexpr auto kSignInPollInterval = std::chrono::milliseconds(500);
// How long to keep in_flight() set after the relaunch, waiting for Steam's second sign-in to
// flush its rotated ConnectCache token (see wait_for_token_flush). Steam doesn't always write
// local.vdf before it exits, so this is expected to time out sometimes; keep it short enough
// that the caller's read-back isn't held up for long when it does.
constexpr auto kTokenFlushTimeout = std::chrono::seconds(30);

// Non-zero while a reapply is scheduled or running (see in_flight()).
std::atomic<int> g_in_flight{0};

// Waits for Steam to report `steam_id_64` as signed in. False on timeout or once the launch
// is superseded. The token path has no login window to watch, so the registry is the signal.
bool wait_for_sign_in(std::uint64_t steam_id_64, int timeout_seconds,
                      const std::function<bool()>& still_current) {
    const auto target = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    for (;;) {
        if (still_current && !still_current()) {
            SAM_LOG_INFO("first-login reapply: superseded while waiting for sign-in");
            return false;
        }
        const auto au = platform::registry::read_active_user();
        if (au && *au == target) return true;
        if (std::chrono::steady_clock::now() >= deadline) {
            SAM_LOG_WARN("first-login reapply: Steam never signed in within {}s; leaving it "
                         "alone (settings apply on the next sign-in)", timeout_seconds);
            return false;
        }
        std::this_thread::sleep_for(kSignInPollInterval);
    }
}

// The relaunch signs in a second time, and Steam rotates the refresh token on every sign-in,
// so the value that was in local.vdf before the relaunch is dead once the new one lands.
// Waits for that write so the caller's watcher adopts the live token instead of the
// superseded one. Best-effort: returns on timeout too, leaving the caller no worse off than
// before this restart existed.
void wait_for_token_flush(const std::string& login_lower,
                          const std::optional<std::string>& before,
                          const std::function<bool()>& still_current) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (still_current && !still_current()) return;   // newer launch owns Steam now
        const auto now_value = steam_local::read_connect_cache_value(login_lower);
        if (now_value && (!before || *now_value != *before)) {
            SAM_LOG_INFO("first-login reapply: relaunch rotated the login token after {}s",
                         std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - start).count());
            return;
        }
        if (std::chrono::steady_clock::now() - start >= kTokenFlushTimeout) {
            SAM_LOG_WARN("first-login reapply: no rotated login token within {}s after the "
                         "relaunch", kTokenFlushTimeout.count());
            return;
        }
        std::this_thread::sleep_for(kSignInPollInterval);
    }
}

}  // namespace

bool in_flight() {
    return g_in_flight.load(std::memory_order_acquire) > 0;
}

void mark_scheduled() {
    g_in_flight.fetch_add(1, std::memory_order_acq_rel);
}

void clear_scheduled() {
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

void run(const Params& p, const std::function<bool()>& still_current) {
    if (p.sign_in_timeout_seconds > 0 &&
        !wait_for_sign_in(p.steam_id_64, p.sign_in_timeout_seconds, still_current))
        return;

    // ActiveUser flips the moment Steam authenticates, long before it creates the account's
    // userdata or writes its login state (loginusers/config.vdf), so stopping Steam right
    // away kills it mid-setup and the relaunch has nothing to sign in with (blank login
    // window). Wait until Steam has set up the account's userdata, grabbing the remember-me
    // token it stored along the way. First login only, so this one-time wait is acceptable.
    SAM_LOG_INFO("first-login reapply: waiting for Steam to finish first-login setup "
                 "before restart");
    std::optional<std::string> cc_token;
    bool setup_ready = false;
    // The driver path can only relaunch into an auto-login with the remember-me token Steam
    // stored, so it waits for one. The token path put its own token in the ConnectCache and
    // whatever Steam leaves there on shutdown is read back below, so it doesn't have to.
    const bool need_cc_token = !p.injected_connect_cache.has_value();
    {
        const auto start = std::chrono::steady_clock::now();
        std::optional<long long> ready_after_s;
        for (;;) {
            if (still_current && !still_current()) {
                SAM_LOG_INFO("first-login reapply: superseded during setup wait; aborting");
                return;
            }
            if (!cc_token) {
                auto value = steam_local::read_connect_cache_value(p.login_lower);
                // Ignore the token we injected ourselves: Steam has already consumed it and
                // rotated it, so only its replacement is worth carrying over the restart.
                if (value && (!p.injected_connect_cache ||
                              *value != *p.injected_connect_cache))
                    cc_token = std::move(value);
            }
            setup_ready = steam_local::login_config_presence(p.steam_id_64).userdata_present;
            const bool ready = setup_ready && (cc_token || !need_cc_token);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (ready && !ready_after_s)
                ready_after_s =
                    std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            if (elapsed >= kMinSetupDwell && ready) break;
            if (elapsed >= kSetupTimeout) break;
            std::this_thread::sleep_for(kSetupPollInterval);
        }
        const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (setup_ready && (cc_token || !need_cc_token))
            SAM_LOG_INFO("first-login reapply: setup ready after {}s, waited {}s; restarting",
                         ready_after_s.value_or(0), waited);
        else if (setup_ready)
            SAM_LOG_WARN("first-login reapply: Steam stored no login token within {}s; "
                         "restarting anyway and writing login state explicitly", waited);
        else
            SAM_LOG_WARN("first-login reapply: userdata not created within {}s; proceeding "
                         "and writing login state explicitly", waited);
    }

    // Steam is now set up. Shut it down (flushes its now-full localconfig/sharedconfig and
    // creates remotecache.vdf), apply the settings in-place so they survive, then relaunch
    // with auto-login. One restart, first login only.
    LaunchResult tmp;
    if (!shutdown_running_steam(p.steam_exe, tmp))
        SAM_LOG_WARN("first-login reapply: shutdown: {}", tmp.message);
    // Let Steam's final config flush settle before we edit and relaunch.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    if (p.apply_launch_options) {
        const auto r = cs2_config::apply_launch_options(p.steam_id_64, p.cs2_launch_options);
        if (!r.ok) SAM_LOG_WARN("first-login reapply: launch options: {}", r.message);
    }
    if (p.apply_news) {
        const auto r = steam_local::set_news_notify_off(p.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("first-login reapply: news: {}", r.message);
    }
    if (p.apply_cloud) {
        // remotecache.vdf now exists, so the cloud write's remotecache refresh can outrank
        // the server copy.
        const auto r = steam_local::set_cloud_enabled_off(p.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("first-login reapply: cloud: {}", r.message);
    }

    // A newer launch may have started during the shutdown; don't relaunch the wrong account
    // or fight its auto-login. The prefs above are already on disk.
    if (still_current && !still_current()) {
        SAM_LOG_INFO("first-login reapply: superseded; skipping relaunch");
        return;
    }
    // Write the full login state ourselves so the relaunch auto-logs in like a token launch,
    // instead of trusting how far Steam's setup got before we stopped it: the remember-me
    // token (only if the shutdown dropped it - Steam rotates the token on sign-in, so the
    // value it left behind is the live one and must not be overwritten with an older one), the
    // loginusers entry, and the config.vdf name -> SteamID map. set_remembered_account is not
    // enough here: it no-ops when Steam hasn't written the loginusers entry yet, which is
    // exactly the first-login race.
    auto cc_before_relaunch = steam_local::read_connect_cache_value(p.login_lower);
    if (!cc_before_relaunch) {
        // Prefer the token Steam rotated to; fall back to the one we injected, which the
        // sign-in has likely superseded, but an empty cache guarantees a login window.
        const auto& restore = cc_token ? cc_token : p.injected_connect_cache;
        if (restore) {
            steam_local::write_connect_cache_raw(p.login_lower, *restore);
            cc_before_relaunch = restore;
            SAM_LOG_INFO("first-login reapply: restored the login token the shutdown dropped");
        }
    }
    steam_local::ensure_loginusers_entry(p.steam_id_64, p.login_lower, "");
    steam_local::ensure_config_vdf_account(p.steam_id_64, p.login_lower);
    if (p.clear_account_chooser && !steam_local::disable_account_chooser())
        SAM_LOG_WARN("first-login reapply: could not clear AlwaysShowUserChooser");
    // Steam account names are ASCII, so a byte-wise widen of the lowercased login is fine.
    platform::registry::set_auto_login_user(
        std::wstring(p.login_lower.begin(), p.login_lower.end()));
    platform::registry::set_remember_password(true);
    auto relaunch_pid = platform::process::launch(p.steam_exe, L"");
    if (!relaunch_pid) {
        SAM_LOG_WARN("first-login reapply: relaunch failed; settings are on disk and apply "
                     "on the next sign-in");
        return;
    }
    SAM_LOG_INFO("first-login reapply: applied prefs and relaunched as {}", p.login_lower);

    if (p.hwid.has_value()) {
        core::Account tmp_acc;
        tmp_acc.hwid = p.hwid;
        tmp_acc.login = p.login_lower;
        maybe_inject_hwid(tmp_acc, *relaunch_pid, p.hwid_mask);
    }

    // Resume the CS2 auto-launch the caller skipped for the deferred case: Steam is back up
    // and signing in on this final instance, so there's no restart left to race it.
    if (p.method != core::LoginMethod::Normal) {
        SAM_LOG_INFO("first-login reapply: starting CS2 autostart post-relaunch");
        cs2_autostart::start_async(p.method, p.steam_id_64, p.gamesense_loader,
                                   p.luminary_loader);
    }

    // Hold in_flight() until the relaunch's sign-in has rotated the stored token, so the
    // caller's read-back lands on the live value rather than the one we just restored.
    wait_for_token_flush(p.login_lower, cc_before_relaunch, still_current);
}

}  // namespace sam::launch::first_login_reapply
