#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/app_state.hpp"
#include "app/settings.hpp"
#include "core/account_store/account.hpp"
#include "core/account_store/ban_event.hpp"

// Shared internals of the AppState translation units (app_state.cpp,
// app_state_refresh.cpp, app_state_login.cpp). Symbols here have external
// linkage in sam::app::detail so they can be defined once (in app_state.cpp)
// and used across TUs without ODR clashes. Single-TU helpers stay in that TU's
// anonymous namespace instead.

namespace sam::app {
namespace detail {

// GCPD scrape is rate-limited independently so the cheap Web API can run more often.
inline constexpr std::int64_t kMinAccountRefreshSeconds = 30;
inline constexpr std::int64_t kMinGcpdRefreshSeconds = 90;

// Steam can flag accounts for rapid persona renames, so lock the action out this long.
inline constexpr std::int64_t kPersonaChangeCooldownSeconds = 300;

// password/tokens are SecureString and self-wipe; the SteamGuard secrets and
// session id are plain std::string, so wipe them before their storage is freed.
void scrub_account_secrets(core::Account& a);

bool event_enabled(const Settings::NotificationToggles& t, core::BanEventKind k);

std::string toast_message_for(const core::Account& a, const core::BanEvent& ev);

void push_toasts_for(AppState& state, const core::Account& a,
                     const std::vector<core::BanEvent>& events,
                     std::int64_t now);

bool ban_event_is_cooldown(core::BanEventKind k);

// One-shot balloon for a manual refresh. Suppressed while the main window is
// focused (the in-app toast covers that).
void push_native_notification(AppState& state, const std::string& message, bool warning);

// Records batch events so they coalesce into one balloon.
void note_session_event(AppState& state, const core::Account& a,
                        const std::vector<core::BanEvent>& events);

// Shows the accumulated batch events as one balloon, then clears the accumulator.
void flush_native_notification(AppState& state);

}  // namespace detail
}  // namespace sam::app
