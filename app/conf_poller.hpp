#pragma once

namespace sam::app {

struct AppState;

}  // namespace sam::app

namespace sam::app::conf_poller {

// Spawns the background polling thread. Reads settings.confirmations.* and
// state.unlocked on each tick. Safe to call once at app startup.
void start(AppState& state);

// Stops the thread and joins. Idempotent.
void stop();

}  // namespace sam::app::conf_poller
