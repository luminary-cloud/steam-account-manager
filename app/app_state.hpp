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

#include "app/settings.hpp"
#include "app/vault_saver.hpp"
#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/notifications/notification_store.hpp"
#include "core/sda/conf_audit.hpp"
#include "core/trade/trade_audit.hpp"
#include "core/update_check.hpp"
#include "ui/widgets/toast_stack.hpp"

// Forward-declare HWND so the entire app/ui doesn't transitively pull in
// <windows.h>. Matches the Windows SDK's own typedef of HWND.
struct HWND__;
using HWND = HWND__*;

namespace sam::app {

enum class Screen {
    Unlock,
    Accounts,
    Authenticator,
    Confirmations,
    TradeOffers,
    AddAccount,
    Settings,
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
    // Set by the account context menu to request the "Change username" modal for
    // selected_account_id; consumed (reset) by the Accounts screen next frame.
    bool persona_change_requested = false;
    // Set by the account-card login-method menu to request a gamesense loader
    // file pick (the per-card popup can't safely host the Win32 dialog).
    // Consumed by the Accounts screen, which runs the dialog at a stable scope.
    // Value is the account id to switch to "CS2 + gamesense" on a successful
    // pick, or an empty string to only (re)install the loader without changing
    // any account's method. nullopt = no pick pending.
    std::optional<std::string> gamesense_pick_request;
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

    // Background GitHub release check kicked off at launch. The worker stores a
    // newer-than-current Result here and draw() pops the "Update available"
    // modal. The jthread requests stop and joins when AppState is destroyed.
    std::mutex update_mutex;
    std::optional<core::update_check::Result> update_result;
    bool update_modal_dismissed_this_session = false;
    std::jthread update_thread;

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

    // Pending incoming trade-offer count surfaced as a badge on the nav rail.
    // Updated by the Trade Offers screen after a refresh; like the confirmations
    // badge we do not poll in the background by default.
    std::atomic<int> pending_trade_offers_count{0};

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
    core::trade::TradeAuditLog trade_audit;
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
    // Headless --startup logon run: refresh_web_phase_done flips when the Web
    // API batch (or an early return) finishes so the run loop knows when to
    // start watching refresh_all_*; balloon_shown lets it linger long enough
    // for a fired notification to stay on screen before exit.
    std::atomic<bool> refresh_web_phase_done{false};
    std::atomic<bool> balloon_shown{false};
    // Accumulates new ban/cooldown events across one batch refresh so they can
    // be coalesced into a single Windows balloon (one balloon shows at a time).
    int session_event_count = 0;
    std::string session_event_message;
    bool session_event_warning = false;

    // Background-thread vault writer (cross-cutting infra: keeps PBKDF2 + AES-
    // GCM off the UI thread so trust label clicks and other rapid mutations
    // don't stall rendering).
    VaultSaver vault_saver;

    // Cross-thread work.
    std::mutex job_mutex;
    std::deque<Job> completed_jobs;
    std::unordered_set<std::string> refreshing_ids;
    // Account ids with an in-flight external-funds (spend) fetch, so the UI can
    // disable the "Refresh spend" action while it runs.
    std::unordered_set<std::string> spend_fetching_ids;

    // Account ids for which we've already fired the "NFA token expired/invalid"
    // notification, so it fires once per dead token. UI-thread only (touched in
    // completed-job callbacks and on import). Cleared on re-import.
    std::unordered_set<std::string> nfa_dead_notified;

    // Refresh rate limiting (per-account-id; populated when a refresh completes).
    // The UI reads these to grey out the Refresh button mid-cooldown.
    std::unordered_map<std::string, std::int64_t> last_refresh_unix;
    std::unordered_map<std::string, std::int64_t> last_gcpd_refresh_unix;
    std::int64_t last_batch_refresh_unix = 0;

    // Per-account cooldown timestamps for display-name changes (unix seconds).
    // Set when a rename succeeds; the Change-username modal greys out Apply
    // until the cooldown elapses.
    std::unordered_map<std::string, std::int64_t> last_persona_change_unix;

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

    // Pushes the current proxy_mode + single_proxy into the http layer's global
    // policy. Call after load_settings and whenever the user changes either.
    void sync_proxy_policy();
    // Spawns the background GitHub release check when check_updates_on_launch is
    // set. No-op otherwise. Result lands in update_result under update_mutex.
    void start_update_check();
    // Drops per-session secrets that should not survive a vault re-lock.
    // Today this is just revealed_logins; extend as new ephemeral reveals land.
    void clear_session_secrets();

    // Securely zero the SteamGuard secrets and session id in the decrypted
    // vault before their storage is freed, so they don't linger in heap memory
    // after a lock or app close. (password/tokens are SecureString already.)
    void scrub_vault_secrets();

    // Remove accounts by id: scrub their secrets, drop them from the vault, and
    // erase their per-account bookkeeping (refresh/persona cooldowns,
    // confirmation state, relogin backoff, selection/reveal sets). Taken by
    // value so callers can pass selected_account_ids directly.
    void remove_accounts(std::unordered_set<std::string> ids);

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

    // Fetches "external funds used" (TotalSpend) for one account. Steam gates
    // help.steampowered.com/accountdata behind a freshly-password-authenticated
    // web:help session, so this performs a full credentials login (auto_relogin,
    // using the stored password + Steam Guard) first, then scrapes AccountSpend
    // with that fresh session. No-op for NFA accounts or accounts without a
    // stored password. `quiet` suppresses per-account toasts (used by the bulk
    // path). Async.
    void refresh_spend(const std::string& id, bool quiet = false);

    // Fetches funds for every eligible account, staggered (one sign-in at a
    // time) to stay gentle on Steam's login rate limit. When `only_missing` is
    // true, skips accounts that already have a figure -- this is the one-time
    // automatic fill on launch. The toolbar "Refresh funds" button passes false.
    void refresh_all_spend(bool only_missing);

    // True while a bulk refresh_all_spend is scheduling/running, so the toolbar
    // can disable the button and show progress.
    std::atomic<bool> spend_bulk_running{false};

    // Copies the stored CS2 video template (app::cs2_video_template_path()) into
    // `a`'s CS2 config folder and pushes a result toast. Surfaces failures as a
    // warning toast rather than throwing. Shared by the login flow and the
    // "Add video config" context-menu item.
    void apply_cs2_video_config(const core::Account& a);

    // Signs the user's default browser in to `a` and opens its Steam profile in
    // a private/incognito window. Mints a fresh web session from the stored
    // refresh_token (re-logging in with the stored password if needed), hands
    // the steamcommunity.com token-transfer target to the browser via a local
    // launcher page, and reports the result as a toast. No-op for NFA accounts.
    void open_account_in_browser(const core::Account& a);

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

    // Returns the seconds remaining before this account's display name can be
    // changed again. 0 means a change is allowed now.
    std::int64_t persona_change_cooldown_seconds(const std::string& id) const;

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
