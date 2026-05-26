#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sam::crypto {

std::string base64_encode(std::span<const std::uint8_t> bytes);
std::string base64_encode(std::string_view bytes);

// Returns empty on malformed input. Whitespace inside the encoded string is ignored.
std::vector<std::uint8_t> base64_decode(std::string_view encoded);

// Percent-encodes a base64 string for use as a query parameter value.
std::string base64_url_query_encode(std::string_view base64);

}  // namespace sam::crypto
