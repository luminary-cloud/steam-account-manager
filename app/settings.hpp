#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sam::app {

enum class AccountsViewMode : std::uint8_t {
    Grid = 0,
    List = 1,
};

// How outbound web requests are proxied. Mirrors http::ProxyMode; kept as a separate
// app-layer enum so Settings doesn't pull in the http header.
enum class ProxyMode : std::uint8_t {
    None = 0,
    Single = 1,
    PerAccount = 2,
};

// What gets copied into the launched account's CS2 (appid 730) config on login.
// None: nothing. VideoTxt: a single cs2_video.txt. Folder730: a whole 730 folder.
enum class CS2ConfigMode : std::uint8_t {
    None = 0,
    VideoTxt = 1,
    Folder730 = 2,
};

struct Settings {
    int clipboard_clear_seconds = 12;
    int auto_lock_minutes = 15;
    AccountsViewMode accounts_view = AccountsViewMode::Grid;
    bool show_avatars = true;
    // Hides account notes everywhere they display (grid cards and the list-mode
    // detail panel). Notes remain editable on the add/edit screen.
    bool hide_notes = false;
    bool refresh_on_launch = false;
    bool gcpd_enabled = true;
    // Caches the master password under the current Windows user via DPAPI so
    // subsequent launches go straight to Accounts. Disabling this option
    // deletes the cached blob.
    bool remember_master_password = false;
    // Registers a Task Scheduler logon task (highest privileges, required since
    // the app is requireAdministrator) that relaunches with --startup to refresh
    // in the background and then exit. Implies remember_master_password +
    // refresh_on_launch so the headless run can auto-unlock and fetch.
    bool start_with_windows = false;
    // When on, account logins render as "<hidden>" everywhere they are
    // displayed. Per-account reveals live in AppState::revealed_logins and
    // are cleared on lock via clear_session_secrets().
    bool privacy_mode = false;
    std::string web_api_key;

    // Outbound proxy policy. single_proxy is used only in Single mode; per-account
    // proxies live on each Account in the encrypted vault.
    ProxyMode   proxy_mode = ProxyMode::None;
    std::string single_proxy;   // scheme://[user:pass@]host:port

    // Checks GitHub for a newer release on launch and shows the "Update
    // available" modal. version_check_skip_until holds the tag the user chose
    // to skip so it is not offered again.
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
        // Out-of-app tray balloon (Shell_NotifyIcon) for new ban/cooldown
        // events; mainly for the headless logon refresh. Suppressed while the
        // main window is focused, where the in-app toast already covers it.
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
        // Hides the login/account name from list-mode rows (persona name and the
        // selected-account detail panel are unaffected).
        bool hide_account_name = false;
    } list_view;

    struct SdaToggles {
        bool auto_copy_on_select = false;
        bool show_next_code = true;
        bool hide_current_code = false;
        bool global_hotkey_enabled = false;
        // Stored as raw Win32 constants so save/load is dead-simple. Defaults
        // are filled in from win_main on first launch since the MOD_* macros
        // live in <windows.h>.
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
        // Destination trade link the create-offer dialog pre-fills, plus the
        // history of links the user has entered. A trade link is not a secret,
        // so these live in settings.json and survive a restart.
        std::string default_destination_trade_url;
        std::vector<std::string> saved_trade_urls;

        // Auto-resolve the mobile confirmation for offers we send. Incoming
        // offers are always reviewed manually.
        bool auto_confirm_sent = true;

        // Trades are rate-limit sensitive: refreshes are manual with a cooldown,
        // multi-account work is staggered, and the inventory endpoint (heavily
        // throttled) gets its own longer cooldown.
        int  per_account_cooldown_seconds = 15;
        int  inventory_cooldown_seconds   = 30;
        int  refresh_stagger_ms           = 1500;

        bool background_poll_enabled      = false;
        int  background_poll_seconds      = 30;
    } trade;

    // Sort key index into the dropdown options; matches core::SortKey order.
    int accounts_sort = 0;

    // List-mode group ids the user has collapsed. Ids not listed are expanded,
    // so new groups default to open; the empty string is the ungrouped section.
    std::vector<std::string> collapsed_groups;

    struct QuickFilters {
        bool only_banned = false;
        bool only_cooldown = false;
        bool only_prime = false;
    } quick_filters;

    struct CS2VideoConfig {
        // What gets applied to the launched account's CS2 730 folder on login.
        // VideoTxt copies the single template at app::cs2_video_template_path();
        // Folder730 copies the whole snapshot under app::cs2_730_template_dir().
        // The *_label fields are the paths the snapshots were imported from and
        // are shown in Settings for reference only.
        CS2ConfigMode mode = CS2ConfigMode::None;
        std::string source_label;         // imported video.txt path
        std::string folder_source_label;  // imported 730 folder path
    } cs2_video;
};

}  // namespace sam::app
