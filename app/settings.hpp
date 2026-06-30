#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sam::app {

enum class AccountsViewMode : std::uint8_t {
    Grid = 0,
    List = 1,
};

// Mirrors http::ProxyMode; separate so Settings doesn't pull in the http header.
enum class ProxyMode : std::uint8_t {
    None = 0,
    Single = 1,
    PerAccount = 2,
};

// How the app signs a password account into the Steam client.
enum class SignInMethod : std::uint8_t {
    Driver = 0,       // UI automation (types password + 2FA into Steam's login window)
    TokenInject = 1,  // inject a client-scoped JWT into Steam's ConnectCache (no login window)
};

// What's copied into the launched account's CS2 (appid 730) config on login.
enum class CS2ConfigMode : std::uint8_t {
    None = 0,
    VideoTxt = 1,
    Folder730 = 2,
};

// What the logon Scheduled Task does, if anything. Only one task ever exists, so
// the two active modes can't collide on the single-instance lock.
enum class LogonAction : std::uint8_t {
    None = 0,
    BackgroundRefresh = 1,  // headless --startup: refresh, notify, exit
    OpenApp = 2,            // launch the full GUI (optionally --minimized)
};

// A saved destination trade link. Name is an optional user label; empty shows the URL.
struct SavedTradeLink {
    std::string url;
    std::string name;
};

struct Settings {
    int clipboard_clear_seconds = 12;
    int auto_lock_minutes = 15;
    AccountsViewMode accounts_view = AccountsViewMode::Grid;
    bool show_avatars = true;
    bool hide_notes = false;
    bool refresh_on_launch = false;
    bool gcpd_enabled = true;
    // Caches the master password via DPAPI (current Windows user). Disabling
    // deletes the cached blob.
    bool remember_master_password = false;
    // Task Scheduler logon task (highest privileges, since requireAdministrator).
    // BackgroundRefresh relaunches with --startup to refresh and exit (implies
    // remember_master_password + refresh_on_launch for the headless auto-unlock);
    // OpenApp launches the full GUI. None removes the task.
    LogonAction logon_action = LogonAction::None;
    // Only meaningful for OpenApp: the logon task passes --minimized so the window
    // opens minimized to the taskbar. Manual launches always open normally.
    bool start_minimized = false;
    // Renders logins as "<hidden>". Per-account reveals live in
    // AppState::revealed_logins, cleared on lock via clear_session_secrets().
    bool privacy_mode = false;
    // Excludes the window from screen-capture software via SetWindowDisplayAffinity
    // (WDA_EXCLUDEFROMCAPTURE). Still visible on the physical monitor.
    bool streamproof = false;
    // Forced to "0" on the launched account during the Steam-down window, only while the
    // toggle is on; never re-enabled. See core/steam_local/login_prefs.
    bool disable_cloud_on_login = false;
    bool disable_news_on_login = false;
    // Whether the password-login driver ticks Steam's "Remember me" box. Off makes Steam
    // forget the session after sign-in (no saved login). Token (NFA) logins ignore this;
    // they require a remembered session to auto-sign-in.
    bool remember_password_on_login = true;
    SignInMethod sign_in_method = SignInMethod::Driver;
    std::string web_api_key;

    // single_proxy used only in Single mode; per-account proxies live in the vault.
    ProxyMode   proxy_mode = ProxyMode::None;
    std::string single_proxy;   // scheme://[user:pass@]host:port

    // version_check_skip_until holds the tag the user chose to skip.
    bool check_updates_on_launch = true;
    std::string version_check_skip_until;

    struct InfoToggles {
        bool show_vac = true;
        bool show_game_ban = true;
        bool show_community_ban = true;
        bool show_trade_ban = true;
        bool show_steam_level = true;
        bool show_owned_games = true;
        bool show_premier = true;
        bool show_wingman = true;
        bool show_prime = true;
        bool show_vac_live = true;
        bool show_cooldown = true;
        bool show_weekly_drop = true;
        bool show_external_funds = true;
    } info;

    struct NotificationToggles {
        bool enabled = true;
        bool surface_in_card = true;
        bool surface_toast = true;
        // Tray balloon (Shell_NotifyIcon), mainly for the headless logon refresh.
        // Suppressed while the main window is focused (in-app toast covers it).
        bool surface_windows_notification = false;
        bool on_new_vac_ban = true;
        bool on_new_game_ban = true;
        bool on_new_community_ban = true;
        bool on_new_trade_ban = true;
        bool on_new_vac_live = true;
        bool on_ban_removed = true;
        bool on_cooldown_started = true;
        bool on_cooldown_ended = true;
        int toast_duration_seconds = 6;
        int coalesce_threshold = 5;
        int retention_days = 30;
    } notifications;

    struct ListViewToggles {
        bool show_cooldown_marker = true;
        bool show_unread_badge = true;
        bool show_weekly_drop_marker = true;
        // Hides login/account name from list-mode rows only.
        bool hide_account_name = false;
    } list_view;

    struct SdaToggles {
        bool auto_copy_on_select = false;
        bool show_next_code = true;
        bool hide_current_code = false;
        bool global_hotkey_enabled = false;
        // Raw Win32 MOD_*/VK constants. Defaults filled from win_main on first
        // launch (the macros live in <windows.h>).
        std::uint32_t global_hotkey_mods = 0;
        std::uint32_t global_hotkey_vk   = 0;
    } sda;

    struct ConfirmationToggles {
        int  per_account_cooldown_seconds = 30;
        int  refresh_stagger_ms           = 250;
        int  bulk_size_cap                = 50;
        int  permanent_failure_threshold  = 3;

        bool background_poll_enabled      = false;
        int  background_poll_minutes      = 10;

        bool toast_on_new_confirmations   = true;
        bool show_account_search          = true;

        bool auto_approve_enabled         = false;
        bool auto_approve_market          = false;
        bool auto_approve_phone_change    = false;
        std::vector<std::uint64_t> auto_approve_trade_partners;

        int  audit_retention_days         = 90;
    } confirmations;

    struct TradeToggles {
        // A trade link is not a secret, so these live in settings.json.
        std::string default_destination_trade_url;
        std::vector<SavedTradeLink> saved_trade_urls;

        // Auto-resolve the mobile confirmation for sent offers; incoming are manual.
        bool auto_confirm_sent = true;

        // Rate-limit sensitive: manual cooldowns, staggered multi-account work,
        // and a longer cooldown for the heavily-throttled inventory endpoint.
        int  per_account_cooldown_seconds = 15;
        int  inventory_cooldown_seconds   = 30;
        int  refresh_stagger_ms           = 1500;

        bool background_poll_enabled      = false;
        int  background_poll_seconds      = 30;
    } trade;

    // Index into the sort dropdown; matches core::SortKey order.
    int accounts_sort = 0;

    // Collapsed list-mode group ids; unlisted = expanded. Empty string = ungrouped.
    std::vector<std::string> collapsed_groups;

    struct QuickFilters {
        bool only_banned = false;
        bool only_cooldown = false;
        bool only_prime = false;
    } quick_filters;

    struct CS2VideoConfig {
        // VideoTxt copies cs2_video_template_path(); Folder730 copies the snapshot
        // under cs2_730_template_dir(). The *_label fields are import paths shown
        // in Settings for reference only.
        CS2ConfigMode mode = CS2ConfigMode::None;
        std::string source_label;         // imported video.txt path
        std::string folder_source_label;  // imported 730 folder path
        // Written into appid 730's LaunchOptions in the launched account's
        // localconfig.vdf, during the Steam-down window so Steam can't clobber it.
        // Empty = leave Steam's existing launch options alone.
        std::string launch_options;
    } cs2_video;

    struct Cs2GcToggles {
        // Master switch: shows the CS2 tab and allows a live Game Coordinator
        // connection. Off hides the tab and closes any active connection.
        bool enabled = true;
        bool show_weekly_drop = true;
        bool show_weekly_mission = true;
        bool show_inventory = true;
        bool show_storage_units = true;
        // Record an observed weekly-drop claim in the vault so the account-list
        // marker lights up without a manual "Mark claimed".
        bool auto_mark_claimed = true;
        // Auto-pull GC data for every eligible account on app startup (self-skips
        // fresh caches, so a quick restart pulls nothing).
        bool auto_pull_on_startup = false;
        // GC pull cache lifetime in hours (1..24); auto-pull skips an account pulled
        // within this window. Persisted in the vault, so it survives restarts.
        int cache_hours = 6;
    } cs2_gc;

    struct HwidToggles {
        bool always_spoof = false;
        std::uint32_t component_mask = 0x3FFu;
    } hwid;
};

}  // namespace sam::app
