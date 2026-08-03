#include "core/trade/trade_link_fetch.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "core/http/client.hpp"
#include "core/log.hpp"

namespace sam::core::trade {

namespace {

const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";

std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
    return s;
}

std::pair<std::string, std::string> parse_set_cookie(const std::string& sc) {
    const std::string first = sc.substr(0, sc.find(';'));
    const auto eq = first.find('=');
    if (eq == std::string::npos) return {std::string{}, std::string{}};
    return {trim(first.substr(0, eq)), trim(first.substr(eq + 1))};
}

std::string origin_of(const std::string& url) {
    const auto p = url.find("://");
    if (p == std::string::npos) return {};
    const auto slash = url.find('/', p + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string host_of(const std::string& url) {
    const auto p = url.find("://");
    const std::size_t start = (p == std::string::npos) ? 0 : p + 3;
    const auto slash = url.find('/', start);
    return url.substr(start, (slash == std::string::npos ? url.size() : slash) - start);
}

std::string resolve_location(const std::string& base, const std::string& loc) {
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) return loc;
    if (loc.rfind("//", 0) == 0) return "https:" + loc;
    if (!loc.empty() && loc.front() == '/') return origin_of(base) + loc;
    return loc;
}

bool is_steam_host(const std::string& host) {
    auto ends_with = [&](const char* suf) {
        const std::string s(suf);
        return host.size() >= s.size() &&
               host.compare(host.size() - s.size(), s.size(), s) == 0;
    };
    return ends_with("steampowered.com") || ends_with("steamcommunity.com");
}

std::string extract_token(const std::string& body) {
    const std::size_t anchor = body.find("trade_offer_access_url");
    const std::size_t from = anchor == std::string::npos ? 0 : anchor;
    const std::size_t tp = body.find("token=", from);
    if (tp == std::string::npos) return {};

    std::string tok;
    for (std::size_t i = tp + 6; i < body.size(); ++i) {
        const char c = body[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) break;
        tok.push_back(c);
    }
    if (tok.size() < 6 || tok.size() > 32) return {};
    return tok;
}

}  // namespace

std::string fetch_trade_link(const core::Account& a, std::string* err) {
    auto fail = [&](std::string msg) -> std::string {
        if (err) *err = std::move(msg);
        return {};
    };

    if (a.steam_id_64 == 0 || a.session_id.empty() || a.steam_login_secure.empty()) {
        SAM_LOG_WARN("tradelink: missing session (steam_id={} sessionid_set={} steam_login_secure_set={})",
                     a.steam_id_64, !a.session_id.empty(), !a.steam_login_secure.empty());
        return fail("no session");
    }

    std::map<std::string, std::string> jar{
        {"sessionid", a.session_id},
        {"steamLoginSecure",
         std::string(a.steam_login_secure.begin(), a.steam_login_secure.end())},
        {"Steam_Language", "english"},
    };

    std::string url = "https://steamcommunity.com/profiles/" +
                      std::to_string(a.steam_id_64) + "/tradeoffers/privacy";
    http::Response resp;
    constexpr int kMaxHops = 6;

    for (int hop = 0; hop <= kMaxHops; ++hop) {
        http::Request req;
        req.method = http::Method::Get;
        req.url = url;
        req.follow_redirects = false;
        req.user_agent = kUserAgent;
        req.headers["Accept-Language"] = "en-US,en;q=0.9";
        req.headers["Accept"] =
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
        for (const auto& [k, v] : jar) {
            req.cookies.push_back({k, v, host_of(url), "/", true, true});
        }

        resp = http::request(req);
        SAM_LOG_INFO("tradelink: hop {} steam_id={} {} -> status={} body={} bytes",
                     hop, a.steam_id_64, url, resp.status, resp.body.size());

        if (resp.transport_error) {
            SAM_LOG_WARN("tradelink: transport error for steam_id={}: {}",
                         a.steam_id_64, resp.error_message);
            return fail(resp.error_message.empty() ? "request failed" : resp.error_message);
        }

        for (const auto& sc : resp.set_cookies) {
            auto [name, value] = parse_set_cookie(sc);
            if (!name.empty() && !value.empty() && value != "deleted") {
                jar[name] = std::move(value);
            }
        }

        const bool is_redirect = resp.status >= 300 && resp.status < 400;
        if (!is_redirect) break;

        const auto it = resp.headers.find("location");
        if (it == resp.headers.end() || it->second.empty()) break;
        const std::string next = resolve_location(url, trim(it->second));
        if (!is_steam_host(host_of(next))) {
            SAM_LOG_WARN("tradelink: refusing off-Steam redirect to '{}'", next);
            return fail("unexpected redirect");
        }
        url = next;
    }

    if (resp.status != 200) {
        SAM_LOG_WARN("tradelink: ended on status {} for steam_id={}",
                     resp.status, a.steam_id_64);
        return fail(resp.status == 0 ? "too many redirects"
                                     : "status " + std::to_string(resp.status));
    }

    const std::string token = extract_token(resp.body);
    if (token.empty()) return fail("not signed in");

    const auto id32 = static_cast<std::uint32_t>(a.steam_id_64 & 0xFFFFFFFFull);
    return "https://steamcommunity.com/tradeoffer/new/?partner=" +
           std::to_string(id32) + "&token=" + token;
}

}  // namespace sam::core::trade
