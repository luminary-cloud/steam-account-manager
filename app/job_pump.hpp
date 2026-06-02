#pragma once

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "app/app_state.hpp"

namespace sam::app::job_pump {

void start_workers(int n = 4);
void stop_workers();

// Enqueues a unit of work that runs on a background thread.
void submit(std::function<void()> task);

// Sleeps up to `total`, waking early once stop_workers() runs. Returns false
// when shutdown was requested so a long staggered task can stop looping.
bool interruptible_sleep(std::chrono::milliseconds total);

// Called once per frame from the main thread to drain completed jobs into
// `state`. Each job's `apply` is invoked while holding no locks.
void drain(AppState& state);

}  // namespace sam::app::job_pump
