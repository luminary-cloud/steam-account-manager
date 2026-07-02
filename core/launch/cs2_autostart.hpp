#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/account_store/account.hpp"

namespace sam::launch::cs2_autostart {

// Spawns a detached worker that waits for sign-in (registry ActiveUser, keyed to
// `steam_id_64` when known), launches CS2 (appid 730) via steam://rungameid/730, and
// for LaunchCs2Gamesense waits for cs2.exe then runs `<loader> --pid=<cs2 pid>
// --load=128`. No-op for LoginMethod::Normal. Everything is captured by value; the
// worker never touches UI/vault state. A newer call supersedes any in-flight worker
// via a generation counter. `gamesense_loader` is ignored unless method is gamesense.
void start_async(core::LoginMethod method, std::uint64_t steam_id_64,
                 std::filesystem::path gamesense_loader,
                 std::filesystem::path luminary_loader = {});

struct StatusMessage {
    std::string text;
    bool warning = false;
};

// Returns and clears the status messages queued since the last call. Call once per
// frame on the UI thread. Thread-safe; the worker only ever touches this static queue
// (never AppState), so it stays valid even if the window is closed mid-autostart.
std::vector<StatusMessage> take_status_messages();

}  // namespace sam::launch::cs2_autostart
