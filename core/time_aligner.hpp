#pragma once

#include <cstdint>

namespace sam::time_aligner {

// Initialises the background sync. Safe to call once at app startup.
// Spawns a worker thread that polls every hour.
void start();

// Stops the worker thread. Safe to call at shutdown.
void stop();

// Returns the current Unix time in seconds, adjusted by the Steam offset.
std::int64_t aligned_now();

// Current offset, in seconds. Positive means Steam clock is ahead of ours.
std::int64_t offset_seconds();

// Has at least one successful sync completed since start()?
bool synced();

// Unix timestamp of the most recent successful sync. 0 if never synced.
std::int64_t last_success_unix();

// 0 if never synced; otherwise the number of seconds since the last success.
std::int64_t seconds_since_last_success();

// Forces an immediate sync on the calling thread. Returns true on success.
bool sync_now();

// Fires sync_now() on a background worker so the UI thread can request a
// fresh sync (e.g. from a "Sync now" button) without blocking.
void sync_now_async();

}  // namespace sam::time_aligner
