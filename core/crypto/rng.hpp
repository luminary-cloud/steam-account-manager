#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sam::crypto {

void fill_random(std::span<std::uint8_t> out);

std::vector<std::uint8_t> random_bytes(std::size_t n);

// Steam-style device id ("android:<uuid>").
std::string random_device_id();

// Hex session id used as the sessionid cookie value.
std::string random_session_id();

}  // namespace sam::crypto
