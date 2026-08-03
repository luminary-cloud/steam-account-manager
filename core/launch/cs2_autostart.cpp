#include "core/launch/cs2_autostart.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include "core/launch/steam_launcher.hpp"
#include "core/log.hpp"
#include "platform/process.hpp"
#include "platform/registry.hpp"

namespace sam::launch::cs2_autostart {

namespace {

using namespace std::chrono_literals;

std::atomic<std::uint64_t> g_gen{0};

std::mutex g_msg_mutex;
std::vector<StatusMessage> g_msgs;

void notify(std::string text, bool warning = false) {
    std::lock_guard lk(g_msg_mutex);
    g_msgs.push_back({std::move(text), warning});
}

constexpr auto kLoginTimeout = 180s;
constexpr auto kLoginPoll    = 500ms;
constexpr auto kCs2Timeout   = 180s;
constexpr auto kCs2Poll      = 1s;
constexpr auto kInjectSettle = 2s;
constexpr int  kLoaderAttempts = 3;

void run_loader(std::uint32_t cs2_pid, const std::filesystem::path& loader) {
    const std::wstring args = L"--pid=" + std::to_wstring(cs2_pid) + L" --load=128";
    for (int attempt = 0; attempt < kLoaderAttempts; ++attempt) {
        if (platform::process::launch(loader, args, loader.parent_path())) {
            SAM_LOG_INFO("cs2_autostart: ran gamesense loader for cs2 pid={}", cs2_pid);
            return;
        }
        SAM_LOG_WARN("cs2_autostart: loader launch failed (attempt {}); retrying",
                     attempt + 1);
        std::this_thread::sleep_for(3s);
    }
    SAM_LOG_ERROR("cs2_autostart: gamesense loader failed after {} attempts",
                  kLoaderAttempts);
    notify("Gamesense loader failed to start.", true);
}

void run_luminary_loader(const std::filesystem::path& loader) {
    const std::wstring args = L"--auto --game=cs2";
    for (int attempt = 0; attempt < kLoaderAttempts; ++attempt) {
        if (platform::process::launch(loader, args, loader.parent_path())) {
            SAM_LOG_INFO("cs2_autostart: ran luminary loader (--auto --game=cs2)");
            return;
        }
        SAM_LOG_WARN("cs2_autostart: luminary loader launch failed (attempt {}); retrying",
                     attempt + 1);
        std::this_thread::sleep_for(3s);
    }
    SAM_LOG_ERROR("cs2_autostart: luminary loader failed after {} attempts",
                  kLoaderAttempts);
    notify("Luminary loader failed to start.", true);
}

void worker_body(std::uint64_t gen, core::LoginMethod method,
                 std::uint64_t steam_id_64, std::filesystem::path loader,
                 std::filesystem::path lum_loader) {
    auto superseded = [gen]() {
        return g_gen.load(std::memory_order_acquire) != gen;
    };

    LaunchResult tmp;
    auto steam_exe = resolve_steam_exe(tmp);
    if (!steam_exe) {
        SAM_LOG_WARN("cs2_autostart: steam.exe not found; skipping");
        notify("Steam install not found; cannot launch CS2.", true);
        return;
    }

    using clk = std::chrono::steady_clock;

    const auto target = static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFull);
    const auto login_deadline = clk::now() + kLoginTimeout;
    bool logged_in = false;
    while (clk::now() < login_deadline) {
        if (superseded()) {
            SAM_LOG_INFO("cs2_autostart: superseded by newer login; exiting");
            return;
        }
        const auto au = platform::registry::read_active_user();
        if (au && *au != 0 && (target == 0 || *au == target)) {
            SAM_LOG_INFO("cs2_autostart: logged in (ActiveUser={})", *au);
            logged_in = true;
            break;
        }
        std::this_thread::sleep_for(kLoginPoll);
    }
    if (!logged_in) {
        SAM_LOG_WARN("cs2_autostart: login did not complete in time; not launching CS2");
        notify("Login timed out - CS2 not launched.", true);
        return;
    }

    if (!platform::process::launch(*steam_exe, L"-- steam://rungameid/730")) {
        SAM_LOG_ERROR("cs2_autostart: failed to launch steam://rungameid/730");
        notify("Failed to launch CS2.", true);
        return;
    }
    SAM_LOG_INFO("cs2_autostart: requested CS2 launch (appid 730)");
    notify("Launching CS2...", false);

    if (method == core::LoginMethod::LaunchCs2Luminary) {
        if (lum_loader.empty()) {
            SAM_LOG_WARN("cs2_autostart: luminary selected but no loader configured; CS2 only");
            notify("No luminary loader configured.", true);
            return;
        }

        const auto cs2_lum_deadline = clk::now() + kCs2Timeout;
        bool cs2_found = false;
        while (clk::now() < cs2_lum_deadline) {
            if (superseded()) {
                SAM_LOG_INFO("cs2_autostart: superseded before cs2.exe appeared; exiting");
                return;
            }
            if (!platform::process::find_by_image_name(L"cs2.exe").empty()) {
                cs2_found = true;
                break;
            }
            std::this_thread::sleep_for(kCs2Poll);
        }
        if (!cs2_found) {
            SAM_LOG_WARN("cs2_autostart: cs2.exe did not appear; skipping luminary");
            notify("CS2 didn't start - luminary not launched.", true);
            return;
        }

        notify("Waiting for CS2 window...", false);
        constexpr auto kWindowTimeout = 120s;
        const auto win_deadline = clk::now() + kWindowTimeout;
        bool window_found = false;
        while (clk::now() < win_deadline) {
            if (superseded()) {
                SAM_LOG_INFO("cs2_autostart: superseded while waiting for CS2 window");
                return;
            }
            bool visible = false;
            EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                if (!IsWindowVisible(hwnd)) return TRUE;
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == 0) return TRUE;
                for (auto cs2_pid : platform::process::find_by_image_name(L"cs2.exe")) {
                    if (pid == cs2_pid) {
                        *reinterpret_cast<bool*>(lp) = true;
                        return FALSE;
                    }
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&visible));
            if (visible) { window_found = true; break; }
            std::this_thread::sleep_for(kCs2Poll);
        }
        if (!window_found)
            SAM_LOG_WARN("cs2_autostart: CS2 window did not appear; launching luminary anyway");

        SAM_LOG_INFO("cs2_autostart: launching luminary loader");
        notify("Launching luminary...", false);
        run_luminary_loader(lum_loader);
        return;
    }

    if (method != core::LoginMethod::LaunchCs2Gamesense) return;

    if (loader.empty()) {
        SAM_LOG_WARN("cs2_autostart: gamesense selected but no loader configured; CS2 only");
        notify("No gamesense loader configured.", true);
        return;
    }

    const auto cs2_deadline = clk::now() + kCs2Timeout;
    std::uint32_t cs2_pid = 0;
    while (clk::now() < cs2_deadline) {
        if (superseded()) {
            SAM_LOG_INFO("cs2_autostart: superseded before cs2.exe appeared; exiting");
            return;
        }
        const auto pids = platform::process::find_by_image_name(L"cs2.exe");
        if (!pids.empty()) { cs2_pid = pids.front(); break; }
        std::this_thread::sleep_for(kCs2Poll);
    }
    if (cs2_pid == 0) {
        SAM_LOG_WARN("cs2_autostart: cs2.exe did not appear; skipping gamesense");
        notify("CS2 didn't start - gamesense not injected.", true);
        return;
    }

    std::this_thread::sleep_for(kInjectSettle);
    SAM_LOG_INFO("cs2_autostart: injecting gamesense into cs2 pid={}", cs2_pid);
    notify("Launching gamesense...", false);
    run_loader(cs2_pid, loader);
}

}  // namespace

void start_async(core::LoginMethod method, std::uint64_t steam_id_64,
                 std::filesystem::path gamesense_loader,
                 std::filesystem::path luminary_loader) {
    if (method == core::LoginMethod::Normal) return;
    const std::uint64_t gen = g_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::thread([gen, method, steam_id_64,
                 loader = std::move(gamesense_loader),
                 lum_loader = std::move(luminary_loader)]() mutable {
        try {
            worker_body(gen, method, steam_id_64, std::move(loader), std::move(lum_loader));
        } catch (...) {
            SAM_LOG_ERROR("cs2_autostart: worker threw");
        }
    }).detach();
}

std::vector<StatusMessage> take_status_messages() {
    std::lock_guard lk(g_msg_mutex);
    return std::exchange(g_msgs, {});
}

}  // namespace sam::launch::cs2_autostart
