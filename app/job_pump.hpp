#pragma once

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "app/app_state.hpp"

namespace sam::app::job_pump {

void start_workers(int n = 4);
void stop_workers();

// Runs `task` on a background worker thread.
void submit(std::function<void()> task);

// Sleeps up to `total`, waking early on stop_workers(). False when shutdown was
// requested so a staggered task can stop looping.
bool interruptible_sleep(std::chrono::milliseconds total);

// Main thread, once per frame: drains completed jobs into `state`. Each `apply`
// runs while holding no locks.
void drain(AppState& state);

}  // namespace sam::app::job_pump
