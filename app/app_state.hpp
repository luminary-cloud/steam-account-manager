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

// Forward declaration keeps <windows.h> out of app/ui. Matches the SDK typedef.
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
    std::function<void()> apply;  // runs on the main thread inside drain()
};

struct AppState {
    bool unlocked = false;
    crypto::SecureString master_password;
    core::Vault vault;

    // Set once by win_main after CreateWindowExW; file-dialog parent.
    HWND main_hwnd = nullptr;

    Screen current_screen = Screen::Unlock;
    std::string selected_account_id;
    std::string search_query;
    // Requests the "Change username" modal; consumed by Accounts next frame.
    bool persona_change_requested = false;
    // Requests a gamesense loader file pick (the per-card popup can't host the
    // Win32 dialog); Accounts runs the dialog at a stable scope. Value is the
    // account id to switch to "CS2 + gamesense", empty = only (re)install the
    // loader, nullopt = no pick pending.
    std::optional<std::string> gamesense_pick_request;
    // Logins currently revealed in privacy_mode.
    std::unordered_set<std::string> revealed_logins;

    // Multi-select on Accounts. Cleared by clear_session_secrets() on lock and by
    // rail_nav when leaving the screen with selection_mode off.
    bool selection_mode = false;
    std::unordered_set<std::string> selected_account_ids;
    std::chrono::steady_clock::time_point last_interaction =
        std::chrono::steady_clock::now();

    Settings settings;
    bool vault_dirty = false;

    // GitHub release check at launch: worker stores a newer Result here, draw()
    // pops the modal. jthread requests stop and joins on AppState destruction.
    std::mutex update_mutex;
    std::optional<core::update_check::Result> update_result;
    bool update_modal_dismissed_this_session = false;
    std::jthread update_thread;

    // Cross-launch index of ban/cooldown events; persisted to notifications.json
    // (non-sensitive, plain JSON).
    core::notifications::NotificationStore notifications;

    // Transient toast stack drained each frame by ui::draw. Workers push via
    // post_ui_callback so the deque stays UI-thread-only.
    ui::widgets::ToastStack toasts;

    // Nav-rail badge count. Not polled in the background; reflects the most recent
    // observed state. `loaded` distinguishes "0 known" from "never queried".
    std::atomic<int> pending_confirmations_count{0};
    std::atomic<bool> pending_confirmations_loaded{false};

    // Nav-rail badge; not polled in the background by default.
    std::atomic<int> pending_trade_offers_count{0};

    // Confirmations-tab per-account state. UI-thread-only writes from post-refresh
    // callbacks; background poller reads relaxed. Distinct from Accounts'
    // last_refresh_unix so each tab's cooldown counts independently.
    std::unordered_map<std::string, std::int64_t> conf_last_refresh_unix;
    std::unordered_map<std::string, int> conf_consecutive_failures;
    std::unordered_set<std::string> conf_permanent_failure;
    std::atomic<int> conf_refresh_all_total{0};
    std::atomic<int> conf_refresh_all_done{0};
    sda::ConfAuditLog conf_audit;
    core::trade::TradeAuditLog trade_audit;
    // pending_confirmations_count at batch start; compared after the last callback
    // to decide whether to toast "N new confirmations".
    std::atomic<int> conf_refresh_batch_baseline{0};

    // Set when global-hotkey settings change so win_main re-registers from the
    // message-loop tick.
    bool needs_hotkey_reregister = true;

    // "Refresh All" progress. Set by refresh_account_data() at batch start;
    // refresh_single_account increments done in its UI callback.
    std::atomic<int> refresh_all_total{0};
    std::atomic<int> refresh_all_done{0};
    // Headless --startup run: refresh_web_phase_done flips when the Web API batch
    // finishes so the loop starts watching refresh_all_*; balloon_shown lets it
    // linger long enough for a fired notification to stay on screen before exit.
    std::atomic<bool> refresh_web_phase_done{false};
    std::atomic<bool> balloon_shown{false};
    // Accumulates new events across one batch so they coalesce into a single
    // Windows balloon (only one shows at a time).
    int session_event_count = 0;
    std::string session_event_message;
    bool session_event_warning = false;

    // Keeps PBKDF2 + AES-GCM off the UI thread so rapid mutations don't stall it.
    VaultSaver vault_saver;

    std::mutex job_mutex;
    std::deque<Job> completed_jobs;
    std::unordered_set<std::string> refreshing_ids;
    // In-flight external-funds (spend) fetches; UI disables "Refresh spend" while set.
    std::unordered_set<std::string> spend_fetching_ids;

    // Ids already notified of a dead NFA token, so it fires once per dead token.
    // UI-thread only (completed-job callbacks and import). Cleared on re-import.
    std::unordered_set<std::string> nfa_dead_notified;

    // Per-account refresh rate limiting; populated on refresh completion. UI reads
    // these to grey out the Refresh button mid-cooldown.
    std::unordered_map<std::string, std::int64_t> last_refresh_unix;
    std::unordered_map<std::string, std::int64_t> last_gcpd_refresh_unix;
    std::int64_t last_batch_refresh_unix = 0;

    // Display-name change cooldowns (unix seconds); modal greys out Apply until elapsed.
    std::unordered_map<std::string, std::int64_t> last_persona_change_unix;

    // WM_DROPFILES enqueues dropped .maFile / info.dat (recursing into dropped
    // dirs); AddAccount drains the matching queue next frame.
    std::mutex drop_mutex;
    std::vector<std::string> pending_mafile_drops;
    bool pending_mafile_focus = false;
    std::vector<std::string> pending_info_dat_drops;
    bool pending_info_dat_focus = false;

    // Set by a worker when a refresh_token is exhausted; next frame pops the Add
    // Account wizard on "Full login" with this login prefilled, then clears it.
    std::optional<std::string> pending_relogin_login;

    // Stops the refresh worker hammering Steam's credential endpoint on a wrong
    // password or rate limit. Worker holds the mutex briefly to read+write.
    std::mutex relogin_mutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        last_relogin_attempt;

    core::Account* find_account(const std::string& id);
    void save_vault_if_dirty();
    void flush_pending_save();
    void save_settings();
    void load_settings();

    // Pushes proxy_mode + single_proxy into the http layer's global policy. Call
    // after load_settings and whenever either changes.
    void sync_proxy_policy();
    // Spawns the GitHub release check if check_updates_on_launch; result lands in
    // update_result under update_mutex.
    void start_update_check();
    // Drops per-session secrets that must not survive a re-lock (revealed_logins).
    void clear_session_secrets();

    // Zero the SteamGuard secrets and session id in the decrypted vault before
    // their storage is freed, so they don't linger in heap after lock/close.
    void scrub_vault_secrets();

    // Scrub secrets, drop from the vault, and erase per-account bookkeeping
    // (cooldowns, confirmation state, relogin backoff, selection/reveal sets).
    // By value so callers can pass selected_account_ids directly.
    void remove_accounts(std::unordered_set<std::string> ids);

    // Tear down the unlocked session: zero the master password, drop the vault,
    // clear session secrets, route back to Unlock. Called by the idle auto-lock.
    void lock_vault();

    void enter_selection_mode();
    void exit_selection_mode();
    void toggle_selected(const std::string& id);
    bool is_selected(const std::string& id) const;
    void refresh_account_data();
    void refresh_single_account(const std::string& id, bool batch_refresh = false);

    // Fetches "external funds used" (TotalSpend). Steam gates accountdata behind
    // a freshly-password-authed web:help session, so this does a full credentials
    // login (auto_relogin) first, then scrapes AccountSpend. No-op for NFA or
    // passwordless accounts. `quiet` suppresses per-account toasts. Async.
    void refresh_spend(const std::string& id, bool quiet = false);

    // Funds for every eligible account, staggered (one sign-in at a time) to stay
    // gentle on Steam's login rate limit. `only_missing` skips accounts with a
    // figure (the one-time launch fill); the toolbar button passes false.
    void refresh_all_spend(bool only_missing);

    // True while a bulk refresh_all_spend runs, for toolbar disable/progress.
    std::atomic<bool> spend_bulk_running{false};

    // Registers/updates/removes the single logon Scheduled Task to match
    // settings.logon_action (+ start_minimized for OpenApp). Idempotent.
    void sync_logon_task() const;

    // Copies cs2_video_template_path() into `a`'s CS2 config folder and toasts the
    // result. Failures toast a warning rather than throw.
    void apply_cs2_video_config(const core::Account& a);

    // Signs the default browser in to `a` and opens its profile in a private
    // window. Mints a fresh web session from the stored refresh_token (relogging
    // in if needed), hands the token-transfer target to the browser via a local
    // launcher page. No-op for NFA accounts.
    void open_account_in_browser(const core::Account& a);

    // Staggered bulk refresh: one outer job walks `ids` and posts
    // refresh_single_account(aid, batch_refresh=true) with `stagger` between them
    // so the worker pool doesn't burst-fan-out Web API requests on bulk imports.
    void refresh_accounts_staggered(
        std::vector<std::string> ids,
        std::chrono::seconds stagger = std::chrono::seconds(2));

    // Seconds until this account can refresh again; 0 = allowed now.
    std::int64_t refresh_cooldown_seconds(const std::string& id) const;

    // Seconds until this account's display name can change again; 0 = allowed now.
    std::int64_t persona_change_cooldown_seconds(const std::string& id) const;

    // Silent credentials re-login: reads login+password+sda from `creds`, runs the
    // mobile auth flow, and on success copies the new tokens back into `creds`.
    // Rate-limited to one attempt / 5 min per account_id. Worker thread only.
    bool auto_relogin(const std::string& account_id,
                      core::Account& creds,
                      std::string* err = nullptr);

    // Seconds left on the 5-min auto-relogin cooldown; 0 = a fresh attempt allowed.
    std::int64_t relogin_cooldown_seconds(const std::string& account_id);

    // Queues `fn` to run on the UI thread in the next drain. Workers use this to
    // mutate the vault safely after a network call.
    void post_ui_callback(std::function<void()> fn);

    std::string launch_error;
};

}  // namespace sam::app
