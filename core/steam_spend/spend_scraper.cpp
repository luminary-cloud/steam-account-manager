#include "core/steam_spend/spend_scraper.hpp"

#include <map>
#include <string>
#include <utility>

#include "core/http/client.hpp"
#include "core/log.hpp"

namespace sam::steam_spend {

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
    if (loc.rfind("//", 0) == 0) return "https:" + loc;             // protocol-relative
    if (!loc.empty() && loc.front() == '/') return origin_of(base) + loc;  // host-relative
    return loc;
}

// Only follow redirects that stay within Steam's web domains, so the session
// cookie can never be replayed to an unrelated host.
bool is_steam_host(const std::string& host) {
    auto ends_with = [&](const char* suf) {
        const std::string s(suf);
        return host.size() >= s.size() &&
               host.compare(host.size() - s.size(), s.size(), s) == 0;
    };
    return ends_with("steampowered.com") || ends_with("steamcommunity.com");
}

}  // namespace

http::Response fetch_account_spend(const core::Account& a) {
    SAM_LOG_INFO("spend: fetching AccountSpend for steam_id={}", a.steam_id_64);

    http::Response empty;
    if (a.steam_id_64 == 0 || a.session_id.empty() || a.steam_login_secure.empty()) {
        SAM_LOG_WARN("spend: missing session (steam_id={} sessionid_set={} steam_login_secure_set={})",
                     a.steam_id_64, !a.session_id.empty(), !a.steam_login_secure.empty());
        empty.error_message = "no session";
        return empty;
    }

    // help.steampowered.com bootstraps its own session via a cross-domain
    // redirect chain that sets cookies along the way. The shared HTTP client
    // disables WinHTTP's cookie store, so we follow the chain ourselves with a
    // per-call jar, honoring Set-Cookie across hops.
    std::map<std::string, std::string> jar{
        {"sessionid", a.session_id},
        {"steamLoginSecure", std::string(a.steam_login_secure.begin(), a.steam_login_secure.end())},
        {"Steam_Language", "english"},
    };
    // The chain's login.steampowered.com/jwt/refresh hop mints the per-domain
    // (web:help) cookie from this refresh-token cookie; without it the hop sets
    // nothing and falls back to the login page. Wire format: <steamid>%7C%7C<jwt>.
    if (!a.refresh_token.empty()) {
        jar["steamRefresh_steam"] = std::to_string(a.steam_id_64) + "%7C%7C" +
            std::string(a.refresh_token.begin(), a.refresh_token.end());
    }

    std::string url = "https://help.steampowered.com/en/accountdata/AccountSpend";
    http::Response resp;
    constexpr int kMaxHops = 10;

    for (int hop = 0; hop <= kMaxHops; ++hop) {
        http::Request req;
        req.method = http::Method::Get;
        req.url = url;
        req.follow_redirects = false;   // we follow manually to preserve Set-Cookie
        req.user_agent = kUserAgent;
        req.headers["Accept-Language"] = "en-US,en;q=0.9";
        req.headers["Accept"] =
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
        for (const auto& [k, v] : jar) {
            req.cookies.push_back({k, v, host_of(url), "/", true, true});
        }

        resp = http::request(req);
        SAM_LOG_INFO("spend: hop {} steam_id={} {} -> status={} body={} bytes set_cookies={}",
                     hop, a.steam_id_64, url, resp.status, resp.body.size(),
                     resp.set_cookies.size());

        if (resp.transport_error) {
            SAM_LOG_WARN("spend: transport error for steam_id={}: {}",
                         a.steam_id_64, resp.error_message);
            return resp;
        }

        for (const auto& sc : resp.set_cookies) {
            auto [name, value] = parse_set_cookie(sc);
            if (!name.empty() && !value.empty() && value != "deleted") {
                jar[name] = std::move(value);
            }
        }

        const bool is_redirect = resp.status >= 300 && resp.status < 400;
        if (!is_redirect) return resp;   // 200 (data or login) or an error

        const auto it = resp.headers.find("location");
        if (it == resp.headers.end() || it->second.empty()) {
            SAM_LOG_WARN("spend: status {} with no Location header, stopping", resp.status);
            return resp;
        }
        const std::string next = resolve_location(url, trim(it->second));
        if (!is_steam_host(host_of(next))) {
            SAM_LOG_WARN("spend: refusing off-Steam redirect to '{}', stopping", next);
            return resp;
        }
        url = next;
    }

    SAM_LOG_WARN("spend: too many redirects for steam_id={}, giving up", a.steam_id_64);
    return resp;
}

}  // namespace sam::steam_spend
