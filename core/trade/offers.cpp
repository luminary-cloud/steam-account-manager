#include "core/trade/offers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/http/client.hpp"
#include "core/log.hpp"
#include "core/steam_api/web_api.hpp"
#include "core/steam_login/session.hpp"
#include "core/trade/trade_url.hpp"

namespace sam::core::trade {

namespace {

using json = nlohmann::json;

std::string header_ci(const http::Response& r, std::string_view name) {
    for (const auto& [k, v] : r.headers) {
        if (k.size() != name.size()) continue;
        if (std::equal(k.begin(), k.end(), name.begin(), [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) ==
                       std::tolower(static_cast<unsigned char>(y));
            })) {
            return v;
        }
    }
    return {};
}

const json& field(const json& j, const char* key) {
    static const json kNull;
    const auto it = j.find(key);
    return it != j.end() ? *it : kNull;
}

// Steam serializes ids as strings but states/timestamps as numbers; accept both.
std::uint64_t to_u64(const json& j) {
    if (j.is_string()) {
        try { return std::stoull(j.get<std::string>()); } catch (...) { return 0; }
    }
    if (j.is_number_unsigned()) return j.get<std::uint64_t>();
    if (j.is_number_integer())  return static_cast<std::uint64_t>(j.get<std::int64_t>());
    return 0;
}

std::int64_t to_i64(const json& j) {
    if (j.is_string()) {
        try { return std::stoll(j.get<std::string>()); } catch (...) { return 0; }
    }
    if (j.is_number()) return j.get<std::int64_t>();
    return 0;
}

bool truthy(const json& j) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number())  return j.get<int>() != 0;
    if (j.is_string())  return j.get<std::string>() == "1";
    return false;
}

std::string desc_key(std::uint64_t appid, std::uint64_t classid, std::uint64_t instanceid) {
    return std::to_string(appid) + "_" + std::to_string(classid) + "_" +
           std::to_string(instanceid);
}

InventoryItem item_from_asset(const json& a,
                              const std::unordered_map<std::string, const json*>& descs) {
    InventoryItem it;
    it.app_id      = static_cast<std::uint32_t>(to_u64(field(a, "appid")));
    it.context_id  = static_cast<std::uint32_t>(to_u64(field(a, "contextid")));
    it.asset_id    = to_u64(field(a, "assetid"));
    it.class_id    = to_u64(field(a, "classid"));
    it.instance_id = to_u64(field(a, "instanceid"));
    it.amount      = static_cast<std::uint32_t>(to_u64(field(a, "amount")));
    if (it.amount == 0) it.amount = 1;

    const auto k = desc_key(it.app_id, it.class_id, it.instance_id);
    if (const auto d = descs.find(k); d != descs.end()) {
        const json& dj = *d->second;
        it.market_hash_name = dj.value("market_hash_name", "");
        it.name             = dj.value("name", "");
        it.type             = dj.value("type", "");
        it.icon_url         = dj.value("icon_url", "");
        it.name_color       = dj.value("name_color", "");
        it.tradable         = truthy(field(dj, "tradable"));
        it.marketable       = truthy(field(dj, "marketable"));
    }
    return it;
}

TradeOffer offer_from_json(const json& o,
                           const std::unordered_map<std::string, const json*>& descs) {
    TradeOffer t;
    const json& id = field(o, "tradeofferid");
    t.offer_id = id.is_string() ? id.get<std::string>() : std::to_string(to_u64(id));
    t.partner_account_id  = static_cast<std::uint32_t>(to_u64(field(o, "accountid_other")));
    t.partner_steam_id_64 =
        t.partner_account_id ? account_id_to_steam_id_64(t.partner_account_id) : 0;
    t.is_our_offer    = truthy(field(o, "is_our_offer"));
    t.state           = static_cast<TradeOfferState>(
        static_cast<int>(to_i64(field(o, "trade_offer_state"))));
    t.message         = o.value("message", "");
    t.expiration_unix = to_i64(field(o, "expiration_time"));
    t.created_unix    = to_i64(field(o, "time_created"));
    t.updated_unix    = to_i64(field(o, "time_updated"));
    t.escrow_end_unix = to_i64(field(o, "escrow_end_date"));
    t.needs_confirmation = (t.state == TradeOfferState::CreatedNeedsConfirmation);

    if (const json& g = field(o, "items_to_give"); g.is_array()) {
        for (const auto& a : g) t.items_to_give.push_back(item_from_asset(a, descs));
    }
    if (const json& r = field(o, "items_to_receive"); r.is_array()) {
        for (const auto& a : r) t.items_to_receive.push_back(item_from_asset(a, descs));
    }
    return t;
}

}  // namespace

TradeOffersResult get_trade_offers(core::Account& a, bool active_only) {
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    TradeOffersResult out;

    if (steam_login::needs_refresh(a, 300)) {
        steam_login::refresh_access_token(a);
    }
    const std::string token(a.access_token.begin(), a.access_token.end());
    if (token.empty()) {
        out.error = "no access token";
        out.needs_relogin = true;
        return out;
    }

    steam_api::WebApiConfig cfg;  // host defaults to api.steampowered.com
    std::map<std::string, std::string> params{
        {"access_token", token},
        {"get_sent_offers", "1"},
        {"get_received_offers", "1"},
        {"get_descriptions", "1"},
        {"language", "english"},
        {"active_only", active_only ? "1" : "0"},
        // Far-future cutoff so active_only=1 returns only currently-active offers,
        // not every offer that has ever changed state (which cutoff 0 would).
        {"time_historical_cutoff", "4000000000"},
    };

    const auto resp = steam_api::get(cfg, "IEconService", "GetTradeOffers", "v1", params, false);
    out.http_status = static_cast<int>(resp.status);

    if (resp.status == 401 || resp.status == 403) {
        out.error = "http " + std::to_string(resp.status);
        out.needs_relogin = true;
        return out;
    }
    if (resp.status != 200) {
        const std::string er = header_ci(resp, "x-eresult");
        out.error = er.empty() ? ("http " + std::to_string(resp.status))
                               : ("http " + std::to_string(resp.status) + " (eresult " + er + ")");
        return out;
    }

    try {
        const auto j = json::parse(resp.body);
        const json& r = field(j, "response");
        if (!r.is_object()) {
            out.error = "unexpected response";
            return out;
        }

        std::unordered_map<std::string, const json*> descs;
        if (const json& ds = field(r, "descriptions"); ds.is_array()) {
            for (const auto& d : ds) {
                descs.emplace(desc_key(to_u64(field(d, "appid")),
                                       to_u64(field(d, "classid")),
                                       to_u64(field(d, "instanceid"))),
                              &d);
            }
        }

        if (const json& s = field(r, "trade_offers_sent"); s.is_array()) {
            for (const auto& o : s) out.sent.push_back(offer_from_json(o, descs));
        }
        if (const json& rc = field(r, "trade_offers_received"); rc.is_array()) {
            for (const auto& o : rc) out.received.push_back(offer_from_json(o, descs));
        }
        out.ok = true;
    } catch (const std::exception& ex) {
        out.error = std::string("parse error: ") + ex.what();
        SAM_LOG_WARN("get_trade_offers: parse failed: {}", ex.what());
    }
    return out;
}

}  // namespace sam::core::trade
