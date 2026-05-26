#include "core/steam_api/web_api.hpp"

#include "core/http/client.hpp"
#include "core/http/url.hpp"

namespace sam::steam_api {

http::Response get(const WebApiConfig& cfg,
                   const std::string& iface,
                   const std::string& method,
                   const std::string& version,
                   std::map<std::string, std::string> params,
                   bool api_key_required) {
    if (api_key_required && !cfg.api_key.empty()) {
        params["key"] = cfg.api_key;
    }
    params["format"] = "json";

    const std::string base =
        cfg.host + "/" + iface + "/" + method + "/" + version + "/";
    const std::string url = http::build_url(base, params);

    http::Request req;
    req.method = http::Method::Get;
    req.url = url;
    req.timeout_seconds = cfg.timeout_seconds;
    return http::request(req);
}

}  // namespace sam::steam_api
