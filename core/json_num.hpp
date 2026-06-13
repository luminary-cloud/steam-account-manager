#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace sam {

// Steam's Web API (and some maFile exporters) serialize numeric fields as JSON
// strings, e.g. "server_time": "1780772474", on which a plain json::value<int>
// throws type_error.302. Accept a number or a numeric string; fall back otherwise.
inline std::int64_t parse_json_i64(const nlohmann::json& v, std::int64_t fallback) {
    if (v.is_number_integer())  return v.get<std::int64_t>();
    if (v.is_number_unsigned()) return static_cast<std::int64_t>(v.get<std::uint64_t>());
    if (v.is_number_float())    return static_cast<std::int64_t>(v.get<double>());
    if (v.is_string()) {
        try { return std::stoll(v.get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

inline std::uint64_t parse_json_u64(const nlohmann::json& v, std::uint64_t fallback = 0) {
    if (v.is_number_unsigned()) return v.get<std::uint64_t>();
    if (v.is_number_integer())  return static_cast<std::uint64_t>(v.get<std::int64_t>());
    if (v.is_number_float())    return static_cast<std::uint64_t>(v.get<double>());
    if (v.is_string()) {
        try { return std::stoull(v.get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

}  // namespace sam
