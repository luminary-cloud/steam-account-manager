#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/http/response.hpp"

namespace sam::http {

enum class Method {
    Get,
    Post,
    Put,
    Delete,
};

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path = "/";
    bool secure = true;
    bool http_only = false;
};

struct Request {
    Method method = Method::Get;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::vector<Cookie> cookies;
    int timeout_seconds = 15;
    int connect_timeout_seconds = 6;
    std::string user_agent =
        "Mozilla/5.0 (Linux; U; Android 9; en-US; "
        "SM-G973U) Valve Steam App Version/3.6.4";
    bool follow_redirects = true;
    int max_retries = 2;
};

// Performs a single request. Retries on transport errors and 5xx responses up to
// `max_retries` with jittered exponential backoff. Never throws.
Response request(const Request& req);

// Aborts all in-flight requests and makes subsequent ones fail immediately.
// Call once at shutdown so blocked worker threads don't wait out the timeout.
void cancel_all();

// How request() picks a proxy. Set process-wide via set_proxy_policy(); defaults to
// Direct so an unconfigured app behaves exactly as it did before proxies existed.
enum class ProxyMode {
    Direct,      // never use a proxy
    Single,      // route every request through `single_proxy`
    PerAccount,  // route through the calling thread's ScopedProxy (per-account)
};

// Installs the global proxy policy. `single_proxy` is only consulted in Single mode.
// Cheap to call repeatedly; the app calls it on settings load and whenever the user
// changes the mode or the single proxy.
void set_proxy_policy(ProxyMode mode, std::string single_proxy);

// RAII guard that records the calling thread's per-account proxy URL
// (scheme://[user:pass@]host:port; socks5, http or https; empty = direct). It only
// takes effect under ProxyMode::PerAccount, so the existing guards can stay in place
// regardless of the active mode. Restores the previous value on destruction so guards
// nest; the thread-local scope keeps concurrent per-account workers isolated.
class ScopedProxy {
public:
    explicit ScopedProxy(std::string proxy_url);
    ~ScopedProxy();
    ScopedProxy(const ScopedProxy&) = delete;
    ScopedProxy& operator=(const ScopedProxy&) = delete;

private:
    std::string prev_;
};

// RAII guard that forces one specific proxy for the calling thread, overriding the
// active ProxyMode entirely. Used by the Settings "Test" buttons to probe a proxy
// before it is saved. Nests and restores like ScopedProxy.
class ScopedProxyOverride {
public:
    explicit ScopedProxyOverride(std::string proxy_url);
    ~ScopedProxyOverride();
    ScopedProxyOverride(const ScopedProxyOverride&) = delete;
    ScopedProxyOverride& operator=(const ScopedProxyOverride&) = delete;

private:
    std::optional<std::string> prev_;
};

// Encodes a flat map as application/x-www-form-urlencoded.
std::string form_encode(const std::map<std::string, std::string>& fields);

}  // namespace sam::http
