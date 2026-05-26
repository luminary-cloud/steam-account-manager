#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sam::crypto {

// Fills the buffer with cryptographically strong random bytes.
// Throws std::runtime_error on failure (which under BCrypt should never happen
// for in-process callers, but the API contract leaves room for it).
void fill_random(std::span<std::uint8_t> out);

std::vector<std::uint8_t> random_bytes(std::size_t n);

// Returns a random Steam-style device id ("android:<uuid>").
std::string random_device_id();

// Returns a random 24-byte hex session id used as the sessionid cookie value.
std::string random_session_id();

}  // namespace sam::crypto
