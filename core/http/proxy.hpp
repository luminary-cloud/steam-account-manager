#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sam::http {

struct ProxyEndpoint {
    std::string scheme;   // lowercased: "socks5", "socks5h", "socks", "http", "https"
    std::string user;
    std::string pass;
    std::string host;
    std::uint16_t port = 0;

    bool is_socks() const {
        return scheme == "socks5" || scheme == "socks5h" || scheme == "socks";
    }
};

// Parses "scheme://[user:pass@]host:port". A trailing path is ignored.
std::optional<ProxyEndpoint> parse_proxy(const std::string& url);

// Returns a "127.0.0.1:<port>" address that fronts the given SOCKS5 upstream as a
// plain HTTP CONNECT proxy. WinHTTP can't speak authenticated SOCKS5, so it points
// here instead and we tunnel its CONNECTs through the real SOCKS5 server (doing the
// username/password handshake on the way). Listeners are cached per upstream and live
// for the process lifetime.
std::optional<std::string> socks_bridge_address(const ProxyEndpoint& upstream);

}  // namespace sam::http
