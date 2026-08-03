#pragma once

#include <optional>

#include "app/app_state.hpp"
#include "core/cleaner/execute.hpp"
#include "core/cleaner/plan.hpp"

namespace sam::app::cleaner_runner {

// Turns Settings::cleaner into an actual run. Everything that needs a clean goes through here
// so the profile lookup, the preserve list and the "Steam must be down" rule live in one place.

enum class Trigger {
    Manual,        // the Run now button
    QuickClean,    // the accounts-screen button: forces the Quick Clean profile and ignores
                   // Settings::cleaner.enabled, since the click is the intent. Still gated
                   // by safe mode.
    BeforeLaunch,  // inside the launch path's Steam-down window
    Unlock,
    Exit,
};

// Resolves the configured profile's targets against this PC and applies the preserve list.
// nullopt when Steam isn't installed. `measure` walks every tree for byte/file totals, which is
// slow enough to be worth skipping outside the preview.
std::optional<cleaner::Plan> build(const Settings& s, bool measure);

// build() + execute(), on the calling thread. Shuts Steam down first unless the caller already
// did (`steam_already_down`, which only the launch path passes), because Steam rewrites
// loginusers.vdf / config.vdf / local.vdf from memory on exit, so a clean while it runs is
// undone moments later.
//
// nullopt when the cleaner is disabled, safe mode is on, Steam isn't installed, Steam couldn't
// be closed, or a first-login reapply is mid-flight (its pending restart would race the wipe).
std::optional<cleaner::CleanResult> run_blocking(const Settings& s, bool steam_already_down,
                                                  bool measure);

// run_blocking() on a worker, storing the outcome in state.cleaner_last and toasting it. Holds
// state.cleaner_busy for the duration; a no-op if a run is already in flight.
void run_async(AppState& state, Trigger why);

}  // namespace sam::app::cleaner_runner
