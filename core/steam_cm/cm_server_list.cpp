#include "core/steam_cm/cm_server_list.hpp"

#include <algorithm>
#include <exception>

#include <nlohmann/json.hpp>

#include "core/http/client.hpp"
#include "core/log.hpp"

namespace sam::steam_cm {

std::vector<CmServer> fetch_cm_list() {
    http::Request req;
    req.method = http::Method::Get;
    req.url = "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/"
              "?cmtype=websockets&format=json";
    // Fail fast on a bad network rather than stacking 15s x retries; a discarded
    // connect is then quick to wind down.
    req.timeout_seconds = 8;
    req.max_retries = 1;

    const auto resp = http::request(req);
    std::vector<CmServer> out;
    if (resp.status != 200 || resp.body.empty()) {
        SAM_LOG_WARN("cm: GetCMListForConnect failed (status {})", resp.status);
        return out;
    }

    try {
        const auto j = nlohmann::json::parse(resp.body);
        for (const auto& s : j.at("response").at("serverlist")) {
            if (s.contains("type") && s.at("type").get<std::string>() != "websockets") continue;
            const std::string endpoint = s.value("endpoint", std::string{});
            const auto colon = endpoint.rfind(':');
            if (colon == std::string::npos) continue;
            CmServer srv;
            srv.host = endpoint.substr(0, colon);
            try {
                srv.port = static_cast<std::uint16_t>(std::stoi(endpoint.substr(colon + 1)));
            } catch (...) {
                continue;
            }
            srv.load = s.value("wtd_load", 0.0);
            out.push_back(std::move(srv));
        }
    } catch (const std::exception& e) {
        SAM_LOG_WARN("cm: failed to parse CM list ({})", e.what());
        return {};
    }

    std::sort(out.begin(), out.end(),
              [](const CmServer& a, const CmServer& b) { return a.load < b.load; });
    SAM_LOG_INFO("cm: {} websocket servers from directory", out.size());
    return out;
}

}  // namespace sam::steam_cm
