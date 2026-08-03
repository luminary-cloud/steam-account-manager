#include "core/http/client.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include "core/http/proxy.hpp"
#include "core/http/url.hpp"
#include "core/log.hpp"

namespace sam::http {

namespace {

std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                       nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string from_wide(const wchar_t* p, std::size_t len) {
    if (!p || len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(len),
                                       nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(len), out.data(), n, nullptr, nullptr);
    return out;
}

const wchar_t* method_w(Method m) {
    switch (m) {
        case Method::Get:    return L"GET";
        case Method::Post:   return L"POST";
        case Method::Put:    return L"PUT";
        case Method::Delete: return L"DELETE";
    }
    return L"GET";
}

int jitter_ms(int base_ms) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> d(0, base_ms);
    return base_ms + d(rng);
}

std::atomic<bool> g_cancelled{false};
thread_local std::string g_thread_proxy;
thread_local std::optional<std::string> g_thread_override;
std::mutex g_policy_mtx;
ProxyMode g_proxy_mode = ProxyMode::Direct;
std::string g_single_proxy;
std::mutex g_session_mtx;
std::map<std::string, HINTERNET> g_sessions;

HINTERNET session_for(const std::string& effective_proxy) {
    std::lock_guard lk(g_session_mtx);
    if (g_cancelled.load(std::memory_order_acquire)) return nullptr;
    if (const auto it = g_sessions.find(effective_proxy); it != g_sessions.end())
        return it->second;

    std::wstring wnamed;
    if (!effective_proxy.empty()) {
        if (const auto ep = parse_proxy(effective_proxy)) {
            if (ep->is_socks()) {
                if (const auto addr = socks_bridge_address(*ep)) wnamed = to_wide(*addr);
                else SAM_LOG_WARN("http: socks bridge unavailable for {}:{}, going direct",
                                  ep->host, ep->port);
            } else if (ep->scheme == "http" || ep->scheme == "https") {
                wnamed = to_wide(ep->host + ":" + std::to_string(ep->port));
            } else {
                SAM_LOG_WARN("http: unsupported proxy scheme '{}', going direct", ep->scheme);
            }
        } else {
            SAM_LOG_WARN("http: ignoring malformed proxy '{}', going direct", effective_proxy);
        }
    }

    HINTERNET session = wnamed.empty()
        ? WinHttpOpen(L"sam/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
        : WinHttpOpen(L"sam/0.1", WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                      wnamed.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        SAM_LOG_WARN("http: WinHttpOpen failed for proxy '{}'", effective_proxy);
        return nullptr;
    }
    g_sessions.emplace(effective_proxy, session);
    return session;
}

struct SessionCleanup {
    ~SessionCleanup() {
        std::lock_guard lk(g_session_mtx);
        for (auto& [key, h] : g_sessions) if (h) WinHttpCloseHandle(h);
        g_sessions.clear();
    }
};
SessionCleanup g_session_cleanup;

void parse_headers(const std::wstring& raw,
                   std::map<std::string, std::string>& out,
                   std::vector<std::string>& cookies_out) {

    std::wstring line;
    bool first = true;
    for (std::size_t i = 0; i <= raw.size(); ++i) {
        const wchar_t c = i < raw.size() ? raw[i] : L'\n';
        if (c == L'\r') continue;
        if (c == L'\n') {
            if (first) {
                first = false;
                line.clear();
                continue;
            }
            if (line.empty()) {
                line.clear();
                continue;
            }
            const auto colon = line.find(L':');
            if (colon != std::wstring::npos) {
                auto name = from_wide(line.data(), colon);
                auto value = from_wide(line.data() + colon + 1, line.size() - colon - 1);
                auto trim = [](std::string& s) {
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
                };
                trim(name);
                trim(value);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == "set-cookie") {
                    cookies_out.push_back(std::move(value));
                } else {
                    out[name] = value;
                }
            }
            line.clear();
            continue;
        }
        line.push_back(c);
    }
}

std::string build_cookie_header(const std::vector<Cookie>& cookies) {
    std::string out;
    for (const auto& c : cookies) {
        if (!out.empty()) out += "; ";
        out += c.name;
        out += '=';
        out += c.value;
    }
    return out;
}

bool split_url(const std::string& url, std::wstring& host, INTERNET_PORT& port,
                std::wstring& path, bool& https) {
    const std::wstring wurl = to_wide(url);
    URL_COMPONENTSW c{};
    c.dwStructSize = sizeof(c);
    wchar_t host_buf[256] = {};
    wchar_t path_buf[2048] = {};
    c.lpszHostName = host_buf;
    c.dwHostNameLength = _countof(host_buf);
    c.lpszUrlPath = path_buf;
    c.dwUrlPathLength = _countof(path_buf);
    c.dwSchemeLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &c)) return false;
    host = std::wstring(c.lpszHostName, c.dwHostNameLength);
    path = std::wstring(c.lpszUrlPath, c.dwUrlPathLength);
    if (path.empty()) path = L"/";
    port = c.nPort;
    https = c.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

std::string resolve_effective_proxy() {
    if (g_thread_override.has_value()) return *g_thread_override;
    std::lock_guard lk(g_policy_mtx);
    switch (g_proxy_mode) {
        case ProxyMode::Direct:     return {};
        case ProxyMode::Single:     return g_single_proxy;
        case ProxyMode::PerAccount: return g_thread_proxy;
    }
    return {};
}

void apply_proxy(HINTERNET request) {
    const std::string proxy = resolve_effective_proxy();
    if (proxy.empty()) return;
    const auto ep = parse_proxy(proxy);
    if (!ep || ep->is_socks()) return;
    if (ep->scheme != "http" && ep->scheme != "https") return;
    if (ep->user.empty() && ep->pass.empty()) return;

    std::wstring wuser = to_wide(ep->user);
    std::wstring wpass = to_wide(ep->pass);
    if (!wuser.empty()) {
        WinHttpSetOption(request, WINHTTP_OPTION_PROXY_USERNAME,
                         wuser.data(), static_cast<DWORD>(wuser.size()));
    }
    if (!wpass.empty()) {
        WinHttpSetOption(request, WINHTTP_OPTION_PROXY_PASSWORD,
                         wpass.data(), static_cast<DWORD>(wpass.size()));
    }
}

Response perform_once(const Request& req) {
    Response out;

    if (g_cancelled.load(std::memory_order_acquire)) {
        out.transport_error = true;
        out.error_message = "cancelled";
        return out;
    }

    const std::string effective_proxy = resolve_effective_proxy();
    HINTERNET session = session_for(effective_proxy);
    if (!session) {
        out.transport_error = true;
        out.error_message = "session unavailable";
        return out;
    }

    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = false;
    if (!split_url(req.url, host, port, path, https)) {
        out.transport_error = true;
        out.error_message = "malformed url";
        return out;
    }

    HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
    if (!conn) {
        out.transport_error = true;
        out.error_message = "WinHttpConnect failed";
        return out;
    }

    const DWORD open_flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(conn,
                                            method_w(req.method),
                                            path.c_str(),
                                            nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            open_flags);
    if (!request) {
        WinHttpCloseHandle(conn);
        out.transport_error = true;
        out.error_message = "WinHttpOpenRequest failed";
        return out;
    }

    const int timeout_ms = req.timeout_seconds * 1000;
    const int connect_ms = req.connect_timeout_seconds * 1000;
    WinHttpSetTimeouts(request, connect_ms, connect_ms, timeout_ms, timeout_ms);

    if (!req.follow_redirects) {
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    }

    {
        DWORD flags = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags));
    }

    apply_proxy(request);

    std::wstring header_block;
    {
        const std::wstring ua = to_wide(req.user_agent);
        header_block += L"User-Agent: " + ua + L"\r\n";
    }
    for (const auto& [k, v] : req.headers) {
        header_block += to_wide(k) + L": " + to_wide(v) + L"\r\n";
    }
    if (!req.cookies.empty()) {
        header_block += L"Cookie: " + to_wide(build_cookie_header(req.cookies)) + L"\r\n";
    }
    if (!header_block.empty()) {
        WinHttpAddRequestHeaders(request, header_block.c_str(),
                                  static_cast<DWORD>(header_block.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    const DWORD body_len = static_cast<DWORD>(req.body.size());
    const BOOL sent = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body_len > 0 ? const_cast<char*>(req.body.data()) : WINHTTP_NO_REQUEST_DATA,
        body_len, body_len, 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        const DWORD err = GetLastError();
        out.transport_error = true;
        out.error_message = "WinHttp send/receive failed (" + std::to_string(err) + ")";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(conn);
        return out;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<long>(status);

    DWORD raw_size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        nullptr, nullptr, &raw_size, WINHTTP_NO_HEADER_INDEX);
    if (raw_size > 0) {
        std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                 nullptr, raw.data(), &raw_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            parse_headers(raw, out.headers, out.set_cookies);
        }
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            out.transport_error = true;
            out.error_message = "WinHttpQueryDataAvailable failed";
            break;
        }
        if (available == 0) break;
        const std::size_t prev = out.body.size();
        out.body.resize(prev + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, out.body.data() + prev, available, &read)) {
            out.transport_error = true;
            out.error_message = "WinHttpReadData failed";
            out.body.resize(prev);
            break;
        }
        out.body.resize(prev + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(conn);
    return out;
}

}  // namespace

Response request(const Request& req) {
    Response last;
    int delay_ms = 200;
    const char* method_u8 = "GET";
    switch (req.method) {
        case Method::Get:    method_u8 = "GET";    break;
        case Method::Post:   method_u8 = "POST";   break;
        case Method::Put:    method_u8 = "PUT";    break;
        case Method::Delete: method_u8 = "DELETE"; break;
    }
    SAM_LOG_DEBUG("http: {} {}", method_u8, req.url);

    for (int attempt = 0; attempt <= req.max_retries; ++attempt) {
        const auto t_start = std::chrono::steady_clock::now();
        last = perform_once(req);
        if (g_cancelled.load(std::memory_order_acquire)) return last;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();

        if (last.transport_error) {
            SAM_LOG_DEBUG("http: {} {} -> transport error '{}' ({} ms, attempt {})",
                          method_u8, req.url, last.error_message, elapsed_ms, attempt);
        } else {
            SAM_LOG_DEBUG("http: {} {} -> {} ({} bytes, {} ms, attempt {})",
                          method_u8, req.url, last.status, last.body.size(),
                          elapsed_ms, attempt);
        }

        const bool transient = last.transport_error ||
                               (last.status >= 500 && last.status <= 599) ||
                               last.status == 429;
        if (!transient || attempt == req.max_retries) {
            if (last.status == 429) {
                SAM_LOG_WARN("http: {} {} -> 429 (giving up after {} attempts)",
                             method_u8, req.url, attempt + 1);
            }
            return last;
        }
        if (last.status == 429) {
            SAM_LOG_WARN("http: {} {} -> 429, will retry", method_u8, req.url);
        }
        int sleep_ms = jitter_ms(delay_ms);
        const auto it = last.headers.find("retry-after");
        if (it != last.headers.end()) {
            try {
                int s = std::stoi(it->second);
                if (s > 0) {
                    if (s > 30) s = 30;
                    sleep_ms = s * 1000;
                }
            } catch (...) {
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        delay_ms *= 2;
    }
    return last;
}

void cancel_all() {
    g_cancelled.store(true, std::memory_order_release);
    std::lock_guard lk(g_session_mtx);
    for (auto& [key, h] : g_sessions) if (h) WinHttpCloseHandle(h);
    g_sessions.clear();
}

std::string current_effective_proxy() {
    return resolve_effective_proxy();
}

ScopedProxy::ScopedProxy(std::string proxy_url) : prev_(std::move(g_thread_proxy)) {
    g_thread_proxy = std::move(proxy_url);
}

ScopedProxy::~ScopedProxy() {
    g_thread_proxy = std::move(prev_);
}

void set_proxy_policy(ProxyMode mode, std::string single_proxy) {
    std::lock_guard lk(g_policy_mtx);
    g_proxy_mode = mode;
    g_single_proxy = std::move(single_proxy);
}

ScopedProxyOverride::ScopedProxyOverride(std::string proxy_url)
    : prev_(std::move(g_thread_override)) {
    g_thread_override = std::move(proxy_url);
}

ScopedProxyOverride::~ScopedProxyOverride() {
    g_thread_override = std::move(prev_);
}

std::string form_encode(const std::map<std::string, std::string>& fields) {
    std::string out;
    bool first = true;
    for (const auto& [k, v] : fields) {
        if (!first) out += '&';
        first = false;
        out += url_encode(k);
        out += '=';
        out += url_encode(v);
    }
    return out;
}

}  // namespace sam::http
