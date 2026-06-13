#include "core/time_aligner.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/http/client.hpp"
#include "core/log.hpp"

namespace sam::time_aligner {

namespace {

std::atomic<std::int64_t> g_offset{0};
std::atomic<std::int64_t> g_last_success_unix{0};
std::atomic<bool> g_synced{false};
std::atomic<bool> g_running{false};

std::mutex g_mtx;
std::condition_variable g_cv;
std::thread g_thread;

constexpr auto kSyncInterval = std::chrono::hours(1);
constexpr auto kRetryInterval = std::chrono::seconds(60);

std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool query_steam_time(std::int64_t& out_server_unix) {
    http::Request req;
    req.method = http::Method::Post;
    req.url = "https://api.steampowered.com/ITwoFactorService/QueryTime/v1/";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = "steamid=0";
    req.timeout_seconds = 8;

    const auto resp = http::request(req);
    if (resp.status != 200) {
        return false;
    }

    try {
        const auto j = nlohmann::json::parse(resp.body);
        const auto& r = j.at("response");
        if (r.contains("server_time")) {
            const auto& st = r["server_time"];
            // QueryTime returns server_time as a string or a number depending on version.
            if (st.is_string()) {
                out_server_unix = std::stoll(st.get<std::string>());
            } else {
                out_server_unix = st.get<std::int64_t>();
            }
            return true;
        }
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("time_aligner: failed to parse response: {}", ex.what());
    }
    return false;
}

void worker_loop() {
    while (g_running.load(std::memory_order_acquire)) {
        const bool ok = sync_now();
        std::unique_lock lk(g_mtx);
        g_cv.wait_for(lk, ok ? kSyncInterval : kRetryInterval,
                      [] { return !g_running.load(std::memory_order_acquire); });
    }
}

}  // namespace

void start() {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) {
        return;
    }
    g_thread = std::thread(worker_loop);
}

void stop() {
    bool expected = true;
    if (!g_running.compare_exchange_strong(expected, false)) {
        return;
    }
    {
        std::lock_guard lk(g_mtx);
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

std::int64_t aligned_now() {
    return now_seconds() + g_offset.load(std::memory_order_acquire);
}

std::int64_t offset_seconds() {
    return g_offset.load(std::memory_order_acquire);
}

bool synced() {
    return g_synced.load(std::memory_order_acquire);
}

std::int64_t seconds_since_last_success() {
    const auto last = g_last_success_unix.load(std::memory_order_acquire);
    if (last == 0) return 0;
    return now_seconds() - last;
}

bool sync_now() {
    std::int64_t server = 0;
    if (!query_steam_time(server)) {
        SAM_LOG_WARN("time_aligner: QueryTime failed");
        return false;
    }
    const auto local = now_seconds();
    const auto new_offset = server - local;
    g_offset.store(new_offset, std::memory_order_release);
    g_last_success_unix.store(local, std::memory_order_release);
    g_synced.store(true, std::memory_order_release);
    SAM_LOG_INFO("time_aligner: synced, offset={}s", new_offset);
    return true;
}

void sync_now_async() {
    std::thread([] { (void)sync_now(); }).detach();
}

}  // namespace sam::time_aligner
