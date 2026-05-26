#include "core/steam_gcpd/gcpd_scraper.hpp"

#include "core/http/client.hpp"
#include "core/log.hpp"

namespace sam::steam_gcpd {

const char* tab_to_string(Tab t) {
    switch (t) {
        case Tab::Matchmaking: return "matchmaking";
        case Tab::AccountMain: return "accountmain";
    }
    return "matchmaking";
}

http::Response fetch_gcpd(const core::Account& a, Tab tab) {
    SAM_LOG_INFO("gcpd: fetching tab='{}' for steam_id={}", tab_to_string(tab), a.steam_id_64);

    http::Response empty;
    if (a.steam_id_64 == 0 || a.session_id.empty() || a.steam_login_secure.empty()) {
        SAM_LOG_WARN("gcpd: missing session (steam_id={} sessionid_set={} steam_login_secure_set={})",
                     a.steam_id_64, !a.session_id.empty(), !a.steam_login_secure.empty());
        empty.error_message = "no session";
        return empty;
    }

    http::Request req;
    req.method = http::Method::Get;
    req.url = "https://steamcommunity.com/profiles/" + std::to_string(a.steam_id_64) +
              "/gcpd/730?tab=" + tab_to_string(tab);
    // Steam rewrites `/profiles/{steamid}/` to `/id/{vanity}/` for accounts
    // that have set a vanity URL. We must follow that redirect to get the
    // actual GCPD HTML. Session-expired detection is content-based at the
    // caller (look for the login page signature in the body).
    req.follow_redirects = true;
    req.user_agent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
    req.headers["Accept-Language"] = "en-US,en;q=0.9";
    req.headers["Accept"] =
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";

    req.cookies.push_back({"sessionid", a.session_id,
                            "steamcommunity.com", "/", true, true});
    req.cookies.push_back({"steamLoginSecure",
                            std::string(a.steam_login_secure.begin(), a.steam_login_secure.end()),
                            "steamcommunity.com", "/", true, true});
    req.cookies.push_back({"Steam_Language", "english",
                            "steamcommunity.com", "/", false, false});

    auto resp = http::request(req);
    SAM_LOG_INFO("gcpd: tab='{}' steam_id={} -> status={} body={} bytes",
                 tab_to_string(tab), a.steam_id_64, resp.status, resp.body.size());
    return resp;
}

}  // namespace sam::steam_gcpd
