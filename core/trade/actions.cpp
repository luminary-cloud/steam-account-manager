#include "core/trade/actions.hpp"

#include <exception>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "core/http/client.hpp"
#include "core/log.hpp"
#include "core/sda/confirmation.hpp"
#include "core/steam_login/web_session.hpp"
#include "core/trade/trade_url.hpp"

namespace sam::core::trade {

namespace {

using json = nlohmann::json;

// Posts a steamcommunity.com tradeoffer action and parses the JSON response. Sets
// needs_relogin on 401/403. Never auto-retries: trade endpoints are aggressively
// rate limited.
OfferActionResult post_action(core::Account& a, const std::string& url,
                              const std::string& referer,
                              std::map<std::string, std::string> body) {
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    OfferActionResult out;
    if (a.is_nfa) {
        out.error = "trades require a full login";
        return out;
    }
    if (!steam_login::ensure_web_session(a)) {
        out.error = "session expired - refresh or re-login this account";
        out.needs_relogin = true;
        return out;
    }

    body["sessionid"] = a.session_id;

    http::Request req;
    req.method = http::Method::Post;
    req.url = url;
    req.max_retries = 0;  // do not hammer a rate-limited trade endpoint
    req.headers["Content-Type"] = "application/x-www-form-urlencoded; charset=UTF-8";
    req.headers["Referer"] = referer;
    req.body = http::form_encode(body);
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
        std::string msg;
        try { msg = json::parse(resp.body).value("strError", ""); } catch (...) {}
        out.error = msg.empty() ? ("http " + std::to_string(resp.status)) : msg;
        return out;
    }

    try {
        const auto j = json::parse(resp.body);
        if (j.contains("strError")) {
            out.error = j["strError"].get<std::string>();
            return out;
        }
        out.needs_confirmation = j.value("needs_mobile_confirmation", false);
        out.ok = true;
    } catch (const std::exception& ex) {
        out.error = "unexpected response from Steam";
        SAM_LOG_WARN("trade action: parse error {} body='{}'", ex.what(), resp.body);
    }
    return out;
}

}  // namespace

OfferActionResult accept_trade_offer(core::Account& a, const std::string& offer_id,
                                     std::uint64_t partner_steam_id_64) {
    const std::string url = "https://steamcommunity.com/tradeoffer/" + offer_id + "/accept";
    const std::string referer = "https://steamcommunity.com/tradeoffer/" + offer_id + "/";
    return post_action(a, url, referer, {
        {"serverid", "1"},
        {"tradeofferid", offer_id},
        {"partner", std::to_string(partner_steam_id_64)},
        {"captcha", ""},
    });
}

OfferActionResult decline_trade_offer(core::Account& a, const std::string& offer_id) {
    const std::string url = "https://steamcommunity.com/tradeoffer/" + offer_id + "/decline";
    const std::string referer = "https://steamcommunity.com/tradeoffer/" + offer_id + "/";
    return post_action(a, url, referer, {});
}

OfferActionResult cancel_trade_offer(core::Account& a, const std::string& offer_id) {
    const std::string url = "https://steamcommunity.com/tradeoffer/" + offer_id + "/cancel";
    const std::string referer = "https://steamcommunity.com/tradeoffer/" + offer_id + "/";
    return post_action(a, url, referer, {});
}

SendOfferResult send_trade_offer(core::Account& a, const TradeUrl& trade_url,
                                 const std::vector<TradeAssetRef>& give,
                                 const std::string& message) {
    http::ScopedProxy proxy_guard(std::string(a.proxy.data(), a.proxy.size()));
    SendOfferResult out;
    if (a.is_nfa) {
        out.error = "trades require a full login";
        return out;
    }
    if (!trade_url.ok) {
        out.error = "invalid trade URL";
        return out;
    }
    if (give.empty()) {
        out.error = "no items selected";
        return out;
    }
    if (!steam_login::ensure_web_session(a)) {
        out.error = "session expired - refresh or re-login this account";
        out.needs_relogin = true;
        return out;
    }

    const std::uint64_t partner_steam = account_id_to_steam_id_64(trade_url.partner_account_id);

    json me_assets = json::array();
    for (const auto& g : give) {
        me_assets.push_back({
            {"appid", g.app_id},
            {"contextid", std::to_string(g.context_id)},
            {"amount", g.amount},
            {"assetid", std::to_string(g.asset_id)},
        });
    }
    const json json_tradeoffer = {
        {"newversion", true},
        {"version", static_cast<int>(give.size()) + 1},
        {"me", {{"assets", me_assets}, {"currency", json::array()}, {"ready", false}}},
        {"them", {{"assets", json::array()}, {"currency", json::array()}, {"ready", false}}},
    };
    const json create_params = {{"trade_offer_access_token", trade_url.token}};

    const std::string referer = "https://steamcommunity.com/tradeoffer/new/?partner=" +
                                std::to_string(trade_url.partner_account_id) +
                                "&token=" + trade_url.token;

    http::Request req;
    req.method = http::Method::Post;
    req.url = "https://steamcommunity.com/tradeoffer/new/send";
    req.max_retries = 0;
    req.headers["Content-Type"] = "application/x-www-form-urlencoded; charset=UTF-8";
    req.headers["Referer"] = referer;
    req.body = http::form_encode({
        {"sessionid", a.session_id},
        {"serverid", "1"},
        {"partner", std::to_string(partner_steam)},
        {"tradeoffermessage", message},
        {"json_tradeoffer", json_tradeoffer.dump()},
        {"captcha", ""},
        {"trade_offer_create_params", create_params.dump()},
    });
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

    try {
        const auto j = json::parse(resp.body);
        if (j.contains("strError")) {
            out.error = j["strError"].get<std::string>();
            return out;
        }
        if (j.contains("tradeofferid")) {
            const auto& id = j["tradeofferid"];
            out.offer_id = id.is_string() ? id.get<std::string>()
                                          : std::to_string(id.get<std::uint64_t>());
            out.needs_confirmation = j.value("needs_mobile_confirmation", false);
            out.ok = true;
            return out;
        }
        out.error = "no tradeofferid in response";
    } catch (const std::exception& ex) {
        out.error = resp.status == 200 ? "unexpected response from Steam"
                                       : ("http " + std::to_string(resp.status));
        SAM_LOG_WARN("send_trade_offer: parse error {} body='{}'", ex.what(), resp.body);
    }
    return out;
}

ConfirmResult confirm_sent_offer(core::Account& a, const std::string& offer_id) {
    ConfirmResult out;
    if (!a.sda.has_value() || a.sda->identity_secret.empty() || a.sda->device_id.empty()) {
        out.error = "no authenticator data for auto-confirm";
        return out;
    }

    const auto fetched = sda::fetch_confirmations(a);
    if (!fetched.ok) {
        out.error = fetched.error.empty() ? "fetch confirmations failed" : fetched.error;
        return out;
    }

    const sda::Confirmation* match = nullptr;
    for (const auto& c : fetched.items) {
        if (c.type == sda::ConfirmationType::Trade && c.creator_id == offer_id) {
            match = &c;
            break;
        }
    }
    if (!match) {
        out.not_found = true;
        return out;
    }

    std::string err;
    if (sda::respond_to_confirmation(a, *match, /*allow=*/true, &err)) {
        out.ok = true;
        return out;
    }
    out.error = err.empty() ? "confirm failed" : err;
    return out;
}

}  // namespace sam::core::trade
