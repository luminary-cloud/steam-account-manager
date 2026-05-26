#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/notifications/notification_store.hpp"
#include "core/sda/conf_audit.hpp"
#include "ui/widgets/toast_stack.hpp"

// Forward-declare HWND so the entire app/ui doesn't transitively pull in
// <windows.h>. Matches the Windows SDK's own typedef of HWND.
struct HWND__;
using HWND = HWND__*;

namespace sam::app {

// Background-thread vault writer. Snapshotting + encryption + atomic write all
// happen off the UI thread; rapid mutations are coalesced into a single save
// via latest-wins debouncing.
class VaultSaver {
public:
    VaultSaver() = default;
    ~VaultSaver();

    VaultSaver(const VaultSaver&) = delete;
    VaultSaver& operator=(const VaultSaver&) = delete;

    // Called once before any schedule(). Idempotent.
    void start(std::filesystem::path path);

    // Snapshot vault + password and schedule an async save. The save fires
    // ~debounce_ms after the most recent schedule() call, so rapid bursts
    // collapse into one write.
    void schedule(const core::Vault& vault,
                  const crypto::SecureString& password);

    // Block until any pending or in-flight save has completed. Safe to call
    // from the UI thread.
    void flush();

private:
    struct Pending {
        core::Vault vault;
        crypto::SecureString password;
        std::chrono::steady_clock::time_point deadline;
    };

    void run();

    std::filesystem::path path_;
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::optional<Pending> pending_;
    bool busy_ = false;
    bool stop_ = false;
    bool started_ = false;
};

enum class Screen {
    Unlock,
    Accounts,
    Authenticator,
    Confirmations,
    AddAccount,
    Settings,
};

enum class AccountsViewMode : std::uint8_t {
    Grid = 0,
    List = 1,
};

struct Settings {
    int clipboard_clear_seconds = 12;
    int auto_lock_minutes = 15;
    AccountsViewMode accounts_view = AccountsViewMode::Grid;
    bool show_avatars = true;
    bool refresh_on_launch = false;
    bool gcpd_enabled = true;
    // Caches the master password under the current Windows user via DPAPI so
    // subsequent launches go straight to Accounts. Disabling this option
    // deletes the cached blob.
    bool remember_master_password = false;
    // When on, account logins render as "<hidden>" everywhere they are
    // displayed. Per-account reveals live in AppState::revealed_logins and
    // are cleared on lock via clear_session_secrets().
    bool privacy_mode = false;
    std::string web_api_key;

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
    } info;

    struct NotificationToggles {
        bool enabled = true;
        bool surface_in_card = true;
        bool surface_toast = true;
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

    // Sort key index into the dropdown options; matches core::SortKey order.
    int accounts_sort = 0;

    struct QuickFilters {
        bool only_banned = false;
        bool only_cooldown = false;
        bool only_prime = false;
    } quick_filters;
};

struct Job {
    std::string account_id;
    std::function<void()> apply;  // Runs on the main thread inside drain().
};

struct AppState {
    // Vault and master password.
    bool unlocked = false;
    crypto::SecureString master_password;
    core::Vault vault;

    // Top-level window handle, set once by win_main after CreateWindowExW. The
    // file-dialog wrapper uses this as the parent so modals stay attached.
    HWND main_hwnd = nullptr;

    // UI state.
    Screen current_screen = Screen::Unlock;
    std::string selected_account_id;
    std::string search_query;
    // Account IDs whose login is currently revealed in privacy_mode.
    std::unordered_set<std::string> revealed_logins;

    // Multi-select on the Accounts screen. selection_mode is the visible
    // toolbar toggle; selected_account_ids holds the checked cards. Cleared by
    // clear_session_secrets() on lock and by rail_nav when leaving the screen
    // with selection_mode off.
    bool selection_mode = false;
    std::unordered_set<std::string> selected_account_ids;
    std::chrono::steady_clock::time_point last_interaction =
        std::chrono::steady_clock::now();

    // Settings.
    Settings settings;
    bool vault_dirty = false;

    // Cross-launch index of detected ban/cooldown events. Persisted to
    // notifications.json (non-sensitive, plain JSON).
    core::notifications::NotificationStore notifications;

    // Transient in-app toast stack drained each frame by ui::draw. Workers
    // push via post_ui_callback so the underlying deque stays UI-thread-only.
    ui::widgets::ToastStack toasts;

    // Pending mobile-confirmation count surfaced as a badge on the nav rail.
    // Updated by the Confirmations screen after a refresh or a submit. We do
    // not poll in the background; the count reflects the most recent observed
    // state and the `loaded` flag distinguishes "0 known" from "never queried".
    std::atomic<int> pending_confirmations_count{0};
    std::atomic<bool> pending_confirmations_loaded{false};

    // Per-account state for the Confirmations tab. UI-thread-only writes from
    // the post-refresh callbacks; background poller reads with a relaxed
    // memory order. Distinct from the Accounts-page `last_refresh_unix` so
    // each tab's cooldown counts independently.
    std::unordered_map<std::string, std::int64_t> conf_last_refresh_unix;
    std::unordered_map<std::string, int> conf_consecutive_failures;
    std::unordered_set<std::string> conf_permanent_failure;
    std::atomic<int> conf_refresh_all_total{0};
    std::atomic<int> conf_refresh_all_done{0};
    sda::ConfAuditLog conf_audit;
    // Snapshot of pending_confirmations_count taken when a refresh-all batch
    // starts; compared after the last per-account callback to decide whether
    // to push a "N new confirmations" toast.
    std::atomic<int> conf_refresh_batch_baseline{0};

    // Set whenever the global-hotkey settings change so win_main can
    // unregister and re-register from the main message-loop tick.
    bool needs_hotkey_reregister = true;

    // Progress counters for the toolbar "Refresh All" chip. Set by
    // refresh_account_data() when the batch starts; refresh_single_account
    // increments done when its UI callback runs.
    std::atomic<int> refresh_all_total{0};
    std::atomic<int> refresh_all_done{0};

    // Background-thread vault writer (cross-cutting infra: keeps PBKDF2 + AES-
    // GCM off the UI thread so trust label clicks and other rapid mutations
    // don't stall rendering).
    VaultSaver vault_saver;

    // Cross-thread work.
    std::mutex job_mutex;
    std::deque<Job> completed_jobs;
    std::unordered_set<std::string> refreshing_ids;

    // Refresh rate limiting (per-account-id; populated when a refresh completes).
    // The UI reads these to grey out the Refresh button mid-cooldown.
    std::unordered_map<std::string, std::int64_t> last_refresh_unix;
    std::unordered_map<std::string, std::int64_t> last_gcpd_refresh_unix;
    std::int64_t last_batch_refresh_unix = 0;

    // Drag-and-drop: WM_DROPFILES handler enqueues every .maFile (and every
    // .maFile found inside a dropped directory) into pending_mafile_drops.
    // Likewise info.dat files go to pending_info_dat_drops. The AddAccount
    // screen drains the matching queue on its next frame.
    std::mutex drop_mutex;
    std::vector<std::string> pending_mafile_drops;
    bool pending_mafile_focus = false;
    std::vector<std::string> pending_info_dat_drops;
    bool pending_info_dat_focus = false;

    // Set by a worker when an account's refresh_token is exhausted/expired and
    // a fresh login is required. The next ImGui frame should pop the Add Account
    // wizard on the "Full login" tab with this login prefilled, then clear it.
    std::optional<std::string> pending_relogin_login;

    // Cooldown to stop the refresh worker from hammering Steam's credential
    // endpoint when a stored password is wrong or the account is rate-limited.
    // Worker holds the mutex briefly to read+write.
    std::mutex relogin_mutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_relogin_attempt;

    core::Account* find_account(const std::string& id);
    void save_vault_if_dirty();
    void flush_pending_save();
    void save_settings();
    void load_settings();
    // Drops per-session secrets that should not survive a vault re-lock.
    // Today this is just revealed_logins; extend as new ephemeral reveals land.
    void clear_session_secrets();

    // Tear down the unlocked session: zero the master password, drop the
    // decrypted vault, clear session secrets, and route the UI back to the
    // Unlock screen. Called by the idle auto-lock check in win_main; a future
    // Lock button should call the same thing.
    void lock_vault();

    void enter_selection_mode();
    void exit_selection_mode();
    void toggle_selected(const std::string& id);
    bool is_selected(const std::string& id) const;
    void refresh_account_data();
    void refresh_single_account(const std::string& id, bool batch_refresh = false);

    // Stagger a refresh across many accounts. Submits one outer job that
    // walks `ids` and posts refresh_single_account(aid, /*batch_refresh=*/true)
    // onto completed_jobs with `stagger` between iterations, so the worker
    // pool doesn't burst-fan-out Web API requests on bulk imports.
    void refresh_accounts_staggered(
        std::vector<std::string> ids,
        std::chrono::seconds stagger = std::chrono::seconds(2));

    // Returns the seconds remaining before this account can be refreshed again
    // (per the per-account rate limit). 0 means the refresh is allowed now.
    std::int64_t refresh_cooldown_seconds(const std::string& id) const;

    // Silent credentials-based re-login. Reads login+password+sda from `creds`,
    // runs the full mobile auth flow, and on success copies the new tokens
    // (access_token, refresh_token, access_token_expires, steam_login_secure,
    // session_id) back into `creds`. Rate-limited to one attempt / 5 min per
    // account_id via last_relogin_attempt. Must run on a worker thread.
    bool auto_relogin(const std::string& account_id,
                      core::Account& creds,
                      std::string* err = nullptr);

    // Returns the seconds remaining on the 5-min auto-relogin cooldown for an
    // account, or 0 if a fresh attempt would be allowed right now. Callers
    // can pre-check this to avoid pointless `auto_relogin` calls.
    std::int64_t relogin_cooldown_seconds(const std::string& account_id);

    // Queues `fn` to run on the UI thread inside the next drain. Worker threads
    // use this to mutate the vault safely after a network call.
    void post_ui_callback(std::function<void()> fn);

    std::string launch_error;
};

}  // namespace sam::app
