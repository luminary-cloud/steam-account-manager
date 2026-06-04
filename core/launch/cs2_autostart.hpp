#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/account_store/account.hpp"

namespace sam::launch::cs2_autostart {

// Spawns a detached worker that finishes what Login started:
//   1. waits for Steam to finish signing in (registry ActiveUser becomes the
//      account id), keyed to `steam_id_64` when known
//   2. launches CS2 (appid 730) via steam://rungameid/730
//   3. for LaunchCs2Gamesense, waits for cs2.exe then runs the loader once as
//      `<loader> --pid=<cs2 pid> --load=128`
//
// No-op for LoginMethod::Normal. Everything is captured by value; the worker
// never touches UI/vault state. A newer call supersedes any in-flight worker
// (generation counter), so logging into another account cancels a pending
// autostart. `gamesense_loader` is ignored unless the method is gamesense.
void start_async(core::LoginMethod method, std::uint64_t steam_id_64,
                 std::filesystem::path gamesense_loader);

// A status update produced by the worker, surfaced to the user as a toast.
struct StatusMessage {
    std::string text;
    bool warning = false;
};

// Returns and clears the status messages queued since the last call. Call once
// per frame on the UI thread and show each as a toast. Thread-safe; the worker
// only ever touches this static queue (never AppState), so it stays valid even
// if the window is closed mid-autostart.
std::vector<StatusMessage> take_status_messages();

}  // namespace sam::launch::cs2_autostart
