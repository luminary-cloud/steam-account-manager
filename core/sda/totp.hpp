#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sam::sda {

// Generates the current 5-character Steam Guard code for `shared_secret_b64`.
// `unix_time` should be the Steam-aligned wall clock in seconds (use TimeAligner).
// Returns an empty string if the secret can't be base64-decoded.
std::string generate_code(std::string_view shared_secret_b64, std::int64_t unix_time);

// Returns the number of seconds remaining in the current 30-second window for
// `unix_time`. Range: (0, 30].
int seconds_remaining(std::int64_t unix_time);

// Convenience: generate using `time_aligner::aligned_now()`.
std::string generate_code_now(std::string_view shared_secret_b64);

}  // namespace sam::sda
