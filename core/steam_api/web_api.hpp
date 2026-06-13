#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "core/http/response.hpp"

namespace sam::steam_api {

struct WebApiConfig {
    std::string api_key;
    std::string host = "https://api.steampowered.com";
    int timeout_seconds = 15;
};

// GET against `interface/method/version/?<params>&key=<api_key>`.
// `api_key_required = false` for unauthenticated endpoints.
http::Response get(const WebApiConfig& cfg,
                   const std::string& iface,
                   const std::string& method,
                   const std::string& version,
                   std::map<std::string, std::string> params,
                   bool api_key_required = true);

// SteamID-shaped JSON value, accepting both numeric and string forms. 0 on failure.
inline std::uint64_t parse_u64(const nlohmann::json& j) {
    if (j.is_string()) {
        try { return std::stoull(j.get<std::string>()); } catch (...) { return 0; }
    }
    if (j.is_number()) return j.get<std::uint64_t>();
    return 0;
}

}  // namespace sam::steam_api
