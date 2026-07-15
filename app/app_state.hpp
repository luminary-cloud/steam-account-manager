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
#include "app/vault_registry.hpp"
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

namespace sam::ui::screens {
struct Cs2ScreenState;
}

namespace sam::cs2_gc {
struct Snapshot;
class Cs2GcClient;
}

namespace sam::app {

enum class Screen {
    Unlock,
    Accounts,
    Authenticator,
    Confirmations,
    TradeOffers,
    Cs2,
    AddAccount,
    Settings,
};

struct Job {
    std::string account_id;
    std::function<void()> apply;  // runs on the main thread inside drain()
};

// State for the CS2 GC auto-pull orchestrator: ONE "puller" account signs in, connects to
// the Game Coordinator, then requests every eligible account's public profile (medals,
// level/XP) by account_id -- one login instead of one-per-account. Mutated only on the UI
// thread (tick + marshaled client callbacks), so it needs no locking.
struct GcAutoPull {
    enum class Phase { Idle, SigningIn, Connecting, Pulling };
    bool active = false;
    Phase phase = Phase::Idle;
    std::string status;  // progress text for the UI

    std::unordered_map<std::uint32_t, std::string> targets;  // 32-bit account id -> vault id
    int total = 0;     // targets at start (progress denominator)
    int received = 0;  // profiles applied (progress numerator)
    int skipped = 0;   // accounts skipped before queueing (info only)

    std::vector<std::string> puller_candidates;  // ordered vault ids to try as the puller
    std::size_t puller_idx = 0;                  // current candidate in puller_candidates
    std::string puller_id;                       // active puller's vault id
    bool signin_pending = false;                 // acquire_cm_token call outstanding
    bool signin_ok = false;
    bool connected = false;   // puller posted on_status == "Ready"
    bool batch_done = false;  // on_profiles_done / on_error fired
    std::unique_ptr<cs2_gc::Cs2GcClient> client;
    std::chrono::steady_clock::time_point phase_started{};
};

// Per-account GC connect that validates an NFA/cached token (via the CM logon result) and
// pulls that account's OWN CS2 profile (ranks/medals/level/cooldown/vac). Processes a queue
// one account at a time -- staggered + TTL-gated so it never trips Steam's rate limiter.
// tick_gc_validate() advances it each frame.
struct GcValidate {
    bool active = false;
    std::string status;  // progress text for the UI
    std::vector<std::string> queue;  // vault ids to process (NFA/cached with a client token)
    std::size_t idx = 0;
    int done = 0;  // finished (progress numerator)

    std::string current_id;
    std::uint32_t current_account_id = 0;  // 32-bit, matches on_profile
    std::unique_ptr<cs2_gc::Cs2GcClient> client;
    bool connected = false;      // puller posted on_status == "Ready"
    bool pull_issued = false;    // own-profile pull_profiles sent
    bool logon_seen = false;     // on_logon fired
    int logon_eresult = 0;       // CM logon EResult (1 = OK)
    bool profile_applied = false;
    bool finished = false;       // on_profiles_done / on_error
    std::chrono::steady_clock::time_point phase_started{};
    std::chrono::steady_clock::time_point resume_at{};  // don't start the next account until here
};

// The account's client-audience refresh token (cm_refresh_token, or the NFA refresh_token
// when it already carries the client audience), or empty when only a mobile/web token is
// available (which the CM rejects). Defined in cs2_autopull.cpp.
std::string cs2_client_token(const core::Account& a);

struct AppState {
    bool unlocked = false;
    crypto::SecureString master_password;
    core::Vault vault;

    // Set once by win_main after CreateWindowExW; file-dialog parent.
    HWND main_hwnd = nullptr;

    Screen current_screen = Screen::Unlock;

    // Multiple vaults: the loaded registry (shared with the picker + settings) and
    // a flag that routes the locked view to the vault picker instead of the unlock
    // screen when several vaults exist and none is set to auto-open.
    VaultRegistry vault_registry;
    bool needs_vault_pick = false;
    // Set by "Switch vault": win_main relaunches the app (--switch) as the very last
    // step of shutdown, after every background thread is joined and the mutex is
    // about to be released, so the replacement never races a slow teardown.
    bool relaunch_switch = false;

    std::string selected_account_id;
    std::string search_query;
    // Shown once per session by the Accounts screen when accounts exist but no
    // Web API key is set; kept dismissible.
    bool warned_missing_api_key = false;
    // When >= 0, draw_settings selects this sub-rail tab on its next frame and
    // resets it to -1. Set by the missing-key toast to jump to Network & Data.
    // Index matches ui::screens::SettingsCategory.
    int pending_settings_category = -1;
    // Requests the "Change username" modal; consumed by Accounts next frame.
    bool persona_change_requested = false;
    // Requests the "Edit notes" modal; consumed by Accounts next frame.
    bool notes_edit_requested = false;
    // Requests the "Edit account" modal; consumed by Accounts next frame.
    bool account_edit_requested = false;
    // Requests a gamesense loader file pick (the per-card popup can't host the
    // Win32 dialog); Accounts runs the dialog at a stable scope. Value is the
    // account id to switch to "CS2 + gamesense", empty = only (re)install the
    // loader, nullopt = no pick pending.
    std::optional<std::string> gamesense_pick_request;
    // Same as above, but for the luminary loader.
    std::optional<std::string> luminary_pick_request;
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

    std::unique_ptr<ui::screens::Cs2ScreenState> cs2_screen;

    // CS2 GC auto-pull orchestrator state + a one-shot guard for the run-on-startup option.
    GcAutoPull gc_autopull;
    bool gc_startup_pull_done = false;

    // Per-account GC validate+pull sweep for NFA/cached accounts (own profile + token check).
    GcValidate gc_validate;
    bool gc_validate_startup_done = false;  // one-shot guard for the run-on-startup validation

    // Cross-launch index of ban/cooldown events; persisted to notifications.json
    // (non-sensitive, plain JSON).
    core::notifications::NotificationStore notifications;

    // Transient toast stack drained each frame by ui::draw. Workers push via
    // post_ui_callback so the deque stays UI-thread-only.
    ui::widgets::ToastStack toasts;

    core::hwid::HwidProfile real_hardware;
    std::string last_hwid_account_id;

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
    // In-flight trade-link fetches, so a double right-click doesn't double-scrape.
    std::unordered_set<std::string> trade_link_fetching_ids;

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

    AppState();
    ~AppState();

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
    // `force` refreshes every account regardless of the Steam cache; the manual "Refresh
    // all" button passes true, while startup/auto pass false so they skip fresh accounts.
    void refresh_account_data(bool force = false);
    // `allow_gcpd` gates the (heavy) GCPD scrape independently of settings.gcpd_enabled;
    // the auto-refresh timer passes false so it only pulls the Steam Web API data.
    void refresh_single_account(const std::string& id, bool batch_refresh = false,
                                bool allow_gcpd = true);

    // Pulls EVERYTHING applicable to a newly added account's type: Steam Web API +
    // (full-access) GCPD + spend + GC, or (NFA/cached) Steam + a GC validate/pull. Used by
    // the add-account flows so a new account fills every field it possibly can.
    void pull_all_for_account(const std::string& id);

    // Auto-refresh sweep for the periodic timer: Steam Web API (no GCPD) for accounts whose
    // data is older than the Steam TTL, plus a TTL-gated GC pull (full-access) and GC
    // validate (NFA/cached). Never spend, never GCPD.
    void auto_refresh_all();

    // Fetches "external funds used" (TotalSpend). Steam gates accountdata behind
    // a freshly-password-authed web:help session, so this does a full credentials
    // login (auto_relogin) first, then scrapes AccountSpend. No-op for NFA or
    // passwordless accounts. `quiet` suppresses per-account toasts. Async.
    void refresh_spend(const std::string& id, bool quiet = false);

    // Copies the account's trade link to the clipboard. Copies the cached value
    // instantly while it is < 7 days old; if missing or stale, scrapes the
    // tradeoffers/privacy page in the background (minting a web session via
    // ensure_web_session), caches the link + a timestamp, and auto-copies the
    // fresh link when it lands. Falls back to the stale cache if a re-fetch fails.
    // No-op for token-only (NFA) accounts that have nothing cached. Async.
    void copy_trade_link(const std::string& id);

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
        std::chrono::seconds stagger = std::chrono::seconds(2),
        bool allow_gcpd = true);

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

    // Mints a SteamClient-audience refresh token (for the CS2 Game Coordinator) via
    // a fresh credentials login, auto-filling Steam Guard from the stored
    // authenticator, and stores it as cm_refresh_token. Async; on_done runs on the
    // UI thread. Requires a stored password.
    void acquire_cm_token(const std::string& account_id,
                          std::function<void(bool, std::string)> on_done);

    // Waits on a worker for Steam to sign this account in, then reads the refresh token
    // Steam rotated into its ConnectCache and writes it back to the account so the stored
    // token stays current. Shows a warning toast if Steam never signs in. Vault writes run
    // on the UI thread via post_ui_callback, and it does nothing if the account is gone.
    void capture_rotated_token_async(std::string account_id,
                                     std::uint64_t steam_id_64,
                                     std::string login_lower);

    // Synchronous version, run just before a launch: reads the token Steam left in its
    // ConnectCache from an earlier sign-in and writes it back to the account, so a rotated
    // token is still picked up when the app was closed before the async version could run.
    // UI thread; does nothing if the account is gone or the stored token is already current.
    void capture_rotated_token_now(const std::string& account_id,
                                   const std::string& login_lower);

    // Caches a GC snapshot's medals (resolved name + icon), level/XP and a pull timestamp
    // into the account's CS2Status, then saves the vault. Shared by the manual CS2 screen
    // and the auto-pull orchestrator.
    void apply_gc_snapshot_cache(const std::string& account_id, const cs2_gc::Snapshot& snap);

    // CS2 GC auto-pull: signs in ONE puller account, connects to the GC, then requests every
    // eligible account's profile (medals + level/XP) by account_id; skips accounts whose cache
    // is still fresh. The puller is auto-picked (never the live Steam account or the manually
    // connected one). tick_gc_autopull() advances it each frame; cancel_gc_autopull() stops it.
    void start_gc_autopull();
    void tick_gc_autopull();
    void cancel_gc_autopull();

    // Per-account GC validate sweep for NFA/cached accounts: each connects with its own
    // client token to validate it (CM logon -> nfa_status) and pull its OWN CS2 profile.
    // `force` ignores the GC cache TTL. `queue_gc_validate` adds one account (used on add).
    // tick_gc_validate() advances the sweep each frame; cancel stops it.
    void start_gc_validate(bool force);
    void queue_gc_validate(const std::string& account_id);
    void tick_gc_validate();
    void cancel_gc_validate();

    // Seconds left on the 5-min auto-relogin cooldown; 0 = a fresh attempt allowed.
    std::int64_t relogin_cooldown_seconds(const std::string& account_id);

    // Queues `fn` to run on the UI thread in the next drain. Workers use this to
    // mutate the vault safely after a network call.
    void post_ui_callback(std::function<void()> fn);

    std::string launch_error;

    struct PendingTokenLaunch {
        std::string account_id;
        bool minting = false;
        bool mint_done = false;
        bool mint_ok = false;
        std::string mint_error;
    } pending_token_launch;
};

// Binds the per-vault stores (notifications + confirmation/trade audits) to the
// now-active vault's folder and loads/prunes them. Call once, right after a vault
// is unlocked (auto-unlock, manual unlock/create, or the picker).
void bind_vault_session(AppState& state);

}  // namespace sam::app
