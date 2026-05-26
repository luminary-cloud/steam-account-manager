#include "app/job_pump.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace sam::app::job_pump {

namespace {

std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_queue;
std::vector<std::thread> g_workers;
std::atomic<bool> g_running{false};

void worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lk(g_mtx);
            g_cv.wait(lk, [] { return !g_queue.empty() || !g_running.load(); });
            if (!g_running.load() && g_queue.empty()) return;
            task = std::move(g_queue.front());
            g_queue.pop_front();
        }
        try {
            task();
        } catch (...) {
            // Workers must never propagate exceptions out; swallow and move on.
        }
    }
}

}  // namespace

void start_workers(int n) {
    if (g_running.exchange(true)) return;
    g_workers.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        g_workers.emplace_back(worker_loop);
    }
}

void stop_workers() {
    if (!g_running.exchange(false)) return;
    g_cv.notify_all();
    for (auto& t : g_workers) {
        if (t.joinable()) t.join();
    }
    g_workers.clear();
}

void submit(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard lk(g_mtx);
        g_queue.push_back(std::move(task));
    }
    g_cv.notify_one();
}

void drain(AppState& state) {
    std::deque<Job> ready;
    {
        std::lock_guard lk(state.job_mutex);
        ready.swap(state.completed_jobs);
    }
    for (auto& j : ready) {
        if (j.apply) j.apply();
    }
}

}  // namespace sam::app::job_pump
