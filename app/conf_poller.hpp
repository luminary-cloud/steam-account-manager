#pragma once

namespace sam::app {

struct AppState;

}  // namespace sam::app

namespace sam::app::conf_poller {

// Spawns the background polling thread; reads settings.confirmations.* and
// state.unlocked each tick. Call once at startup.
void start(AppState& state);

// Stops and joins. Idempotent.
void stop();

}  // namespace sam::app::conf_poller
