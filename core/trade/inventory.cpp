#include "core/trade/inventory.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/http/client.hpp"
#include "core/log.hpp"
#include "core/steam_login/web_session.hpp"

namespace sam::core::trade {

namespace {

using json = nlohmann::json;

const json& field(const json& j, const char* key) {
    static const json kNull;
    const auto it = j.find(key);
    return it != j.end() ? *it : kNull;
}

std::uint64_t to_u64(const json& j) {
    if (j.is_string()) {
        try { return std::stoull(j.get<std::string>()); } catch (...) { return 0; }
    }
    if (j.is_number_unsigned()) return j.get<std::uint64_t>();
    if (j.is_number_integer())  return static_cast<std::uint64_t>(j.get<std::int64_t>());
    return 0;
}

bool truthy(const json& j) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number())  return j.get<int>() != 0;
    if (j.is_string())  return j.get<std::string>() == "1";
    return false;
}

std::string key3(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    return std::to_string(a) + "_" + std::to_string(b) + "_" + std::to_string(c);
}

}  // namespace

InventoryFetchResult fetch_inventory(core::Account& a, std::uint64_t owner_steam_id_64,
                                     std::uint32_t app_id, std::uint32_t context_id,
                                     int count, std::uint64_t start_assetid) {
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    InventoryFetchResult out;

    if (!steam_login::ensure_web_session(a)) {
        out.error = "session expired - refresh or re-login this account";
        out.needs_relogin = true;
        return out;
    }

    std::string url = "https://steamcommunity.com/inventory/" +
                      std::to_string(owner_steam_id_64) + "/" +
                      std::to_string(app_id) + "/" + std::to_string(context_id) +
                      "?l=english&count=" + std::to_string(count);
    if (start_assetid != 0) url += "&start_assetid=" + std::to_string(start_assetid);

    http::Request req;
    req.method = http::Method::Get;
    req.url = url;
    req.max_retries = 1;
    req.headers["Referer"] = "https://steamcommunity.com/profiles/" +
                             std::to_string(owner_steam_id_64) + "/inventory";
    req.cookies.push_back({"steamLoginSecure", steam_login::web_login_secure_cookie(a),
                           "steamcommunity.com", "/", true, true});
    req.cookies.push_back({"sessionid", a.session_id,
                           "steamcommunity.com", "/", true, true});

    const auto resp = http::request(req);
    out.http_status = static_cast<int>(resp.status);
    if (resp.status == 401 || resp.status == 403) {
        out.error = "http " + std::to_string(resp.status);
        out.needs_relogin = true;
        return out;
    }
    if (resp.status == 429) {
        out.error = "rate limited by Steam (429)";
        return out;
    }
    if (resp.status != 200) {
        out.error = "http " + std::to_string(resp.status);
        return out;
    }

    try {
        const auto j = json::parse(resp.body);
        const json& assets = field(j, "assets");
        const json& descriptions = field(j, "descriptions");
        if (!assets.is_array() || assets.empty() || !descriptions.is_array()) {

            out.ok = truthy(field(j, "success")) ||
                     field(j, "total_inventory_count").is_number();
            if (!out.ok) out.error = "inventory unavailable (private?)";
            return out;
        }

        std::unordered_map<std::string, const json*> descs;
        for (const auto& d : descriptions) {
            descs.emplace(key3(to_u64(field(d, "appid")),
                               to_u64(field(d, "classid")),
                               to_u64(field(d, "instanceid"))),
                          &d);
        }

        for (const auto& as : assets) {
            InventoryItem it;
            it.app_id      = static_cast<std::uint32_t>(to_u64(field(as, "appid")));
            it.context_id  = static_cast<std::uint32_t>(to_u64(field(as, "contextid")));
            it.asset_id    = to_u64(field(as, "assetid"));
            it.class_id    = to_u64(field(as, "classid"));
            it.instance_id = to_u64(field(as, "instanceid"));
            it.amount      = static_cast<std::uint32_t>(to_u64(field(as, "amount")));
            if (it.amount == 0) it.amount = 1;
            if (const auto d = descs.find(key3(it.app_id, it.class_id, it.instance_id));
                d != descs.end()) {
                const json& dj = *d->second;
                it.market_hash_name = dj.value("market_hash_name", "");
                it.name             = dj.value("name", "");
                it.type             = dj.value("type", "");
                it.icon_url         = dj.value("icon_url", "");
                it.name_color       = dj.value("name_color", "");
                it.tradable         = truthy(field(dj, "tradable"));
                it.marketable       = truthy(field(dj, "marketable"));
            }
            out.items.push_back(std::move(it));
        }
        out.more = truthy(field(j, "more_items"));
        out.last_assetid = to_u64(field(j, "last_assetid"));
        out.ok = true;
    } catch (const std::exception& ex) {
        out.error = std::string("parse error: ") + ex.what();
        SAM_LOG_WARN("fetch_inventory: parse failed: {}", ex.what());
    }
    return out;
}

}  // namespace sam::core::trade
