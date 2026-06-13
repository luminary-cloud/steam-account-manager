#pragma once

#include <cstdint>

namespace sam::time_aligner {

void start();
void stop();

std::int64_t aligned_now();

// Positive means the Steam clock is ahead of ours.
std::int64_t offset_seconds();

bool synced();
std::int64_t seconds_since_last_success();

bool sync_now();
void sync_now_async();

}  // namespace sam::time_aligner
