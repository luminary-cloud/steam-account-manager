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

constexpr auto kTokenFlushTimeout = std::chrono::seconds(30);

std::atomic<int> g_in_flight{0};

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

void wait_for_token_flush(const std::string& login_lower,
                          const std::optional<std::string>& before,
                          const std::function<bool()>& still_current) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (still_current && !still_current()) return;
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

DeferredPrefs apply_login_prefs(std::uint64_t steam_id_64, const LaunchOptions& opts,
                                const steam_local::LoginConfigPresence& presence,
                                std::string_view log_tag) {

    DeferredPrefs deferred;
    deferred.launch_options = !opts.cs2_launch_options.empty() && !presence.localconfig_present;
    deferred.news           = opts.disable_news_on_login && !presence.localconfig_present;
    deferred.persona        = opts.login_invisible && !presence.localconfig_present;
    deferred.remote_play    = opts.disable_remote_play_on_login && !presence.localconfig_present;
    deferred.cloud          = opts.disable_cloud_on_login && !presence.sharedconfig_present;

    if (!opts.cs2_launch_options.empty() && !deferred.launch_options) {
        const auto r =
            cs2_config::apply_launch_options(steam_id_64, opts.cs2_launch_options);
        if (!r.ok) SAM_LOG_WARN("{}: cs2 launch options: {}", log_tag, r.message);
    }
    {

        steam_local::LocalConfigPrefs local;
        local.news_notify_off   = opts.disable_news_on_login && !deferred.news;
        local.persona_invisible = opts.login_invisible && !deferred.persona;
        local.remote_play_off   = opts.disable_remote_play_on_login && !deferred.remote_play;
        const auto r = steam_local::apply_localconfig_prefs(steam_id_64, local);
        if (!r.ok) SAM_LOG_WARN("{}: localconfig prefs: {}", log_tag, r.message);
    }
    if (opts.disable_cloud_on_login && !deferred.cloud) {
        const auto r = steam_local::set_cloud_enabled_off(steam_id_64);
        if (!r.ok) SAM_LOG_WARN("{}: disable cloud: {}", log_tag, r.message);
    }

    return deferred;
}

void run(const Params& p, const std::function<bool()>& still_current) {
    if (p.sign_in_timeout_seconds > 0 &&
        !wait_for_sign_in(p.steam_id_64, p.sign_in_timeout_seconds, still_current))
        return;

    SAM_LOG_INFO("first-login reapply: waiting for Steam to finish first-login setup "
                 "before restart");
    std::optional<std::string> cc_token;
    bool setup_ready = false;

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

    LaunchResult tmp;
    if (!shutdown_running_steam(p.steam_exe, tmp))
        SAM_LOG_WARN("first-login reapply: shutdown: {}", tmp.message);

    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    if (p.deferred.launch_options) {
        const auto r = cs2_config::apply_launch_options(p.steam_id_64, p.cs2_launch_options);
        if (!r.ok) SAM_LOG_WARN("first-login reapply: launch options: {}", r.message);
    }
    {
        steam_local::LocalConfigPrefs local;
        local.news_notify_off   = p.deferred.news;
        local.persona_invisible = p.deferred.persona;
        local.remote_play_off   = p.deferred.remote_play;
        const auto r = steam_local::apply_localconfig_prefs(p.steam_id_64, local);
        if (!r.ok) SAM_LOG_WARN("first-login reapply: localconfig prefs: {}", r.message);
    }
    if (p.deferred.cloud) {

        const auto r = steam_local::set_cloud_enabled_off(p.steam_id_64);
        if (!r.ok) SAM_LOG_WARN("first-login reapply: cloud: {}", r.message);
    }

    if (still_current && !still_current()) {
        SAM_LOG_INFO("first-login reapply: superseded; skipping relaunch");
        return;
    }

    auto cc_before_relaunch = steam_local::read_connect_cache_value(p.login_lower);
    if (!cc_before_relaunch) {

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

    if (p.method != core::LoginMethod::Normal) {
        SAM_LOG_INFO("first-login reapply: starting CS2 autostart post-relaunch");
        cs2_autostart::start_async(p.method, p.steam_id_64, p.gamesense_loader,
                                   p.luminary_loader);
    }

    wait_for_token_flush(p.login_lower, cc_before_relaunch, still_current);
}

}  // namespace sam::launch::first_login_reapply
