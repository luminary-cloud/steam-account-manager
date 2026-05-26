#include "app/conf_poller.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "app/app_state.hpp"
#include "core/log.hpp"
#include "ui/screens/confirmations_screen.hpp"

namespace sam::app::conf_poller {

namespace {

std::atomic<bool> g_running{false};
std::mutex g_mtx;
std::condition_variable g_cv;
std::thread g_thread;
AppState* g_state = nullptr;

void worker_loop() {
    while (g_running.load(std::memory_order_acquire)) {
        const int minutes = g_state
            ? g_state->settings.confirmations.background_poll_minutes
            : 10;
        const auto wait = std::chrono::minutes(minutes > 0 ? minutes : 10);

        std::unique_lock lk(g_mtx);
        g_cv.wait_for(lk, wait,
                       [] { return !g_running.load(std::memory_order_acquire); });
        if (!g_running.load(std::memory_order_acquire)) return;
        lk.unlock();

        if (!g_state) continue;
        const auto& cfg = g_state->settings.confirmations;
        if (!cfg.background_poll_enabled) continue;
        if (!g_state->unlocked) continue;
        if (g_state->conf_refresh_all_total.load(std::memory_order_relaxed) > 0 &&
            g_state->conf_refresh_all_done.load(std::memory_order_relaxed) <
            g_state->conf_refresh_all_total.load(std::memory_order_relaxed)) {
            continue;
        }

        SAM_LOG_INFO("conf_poller: tick fired, queuing refresh_all");
        g_state->post_ui_callback([] {
            if (g_state) ui::screens::confirmations_trigger_refresh_all(*g_state);
        });
    }
}

}  // namespace

void start(AppState& state) {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    g_state = &state;
    g_thread = std::thread(worker_loop);
}

void stop() {
    bool expected = true;
    if (!g_running.compare_exchange_strong(expected, false)) return;
    {
        std::lock_guard lk(g_mtx);
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    g_state = nullptr;
}

}  // namespace sam::app::conf_poller
