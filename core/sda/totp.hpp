#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sam::sda {

// `unix_time` must be the Steam-aligned wall clock in seconds (use TimeAligner).
// Returns empty if the secret can't be base64-decoded.
std::string generate_code(std::string_view shared_secret_b64, std::int64_t unix_time);

// Seconds remaining in the current 30-second window. Range: (0, 30].
int seconds_remaining(std::int64_t unix_time);

std::string generate_code_now(std::string_view shared_secret_b64);

}  // namespace sam::sda
