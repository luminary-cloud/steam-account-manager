#include "core/steam_cm/ws_transport.hpp"

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <chrono>

#include "core/http/proxy.hpp"
#include "core/log.hpp"

namespace sam::steam_cm {

namespace {

// Effectively infinite: the reader blocks until a real message or a close/abort, so
// it must never hit a receive timeout (that aborts the WinHTTP web socket).
constexpr int kNoTimeoutMs = 0x7FFFFFFF;
constexpr int kSendTimeoutMs = 15000;

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// Mirrors http::session_for: the proxy must be baked into the session, with a
// SOCKS5 upstream fronted by the local CONNECT bridge.
HINTERNET make_session(const std::string& effective_proxy) {
    std::wstring named;
    if (!effective_proxy.empty()) {
        if (const auto ep = http::parse_proxy(effective_proxy)) {
            if (ep->is_socks()) {
                if (const auto addr = http::socks_bridge_address(*ep)) named = to_wide(*addr);
                else SAM_LOG_WARN("cm: socks bridge unavailable, going direct");
            } else if (ep->scheme == "http" || ep->scheme == "https") {
                named = to_wide(ep->host + ":" + std::to_string(ep->port));
            }
        }
    }
    return named.empty()
        ? WinHttpOpen(L"sam/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
        : WinHttpOpen(L"sam/0.1", WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                      named.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
}

void apply_proxy_creds(HINTERNET request, const std::string& effective_proxy) {
    if (effective_proxy.empty()) return;
    const auto ep = http::parse_proxy(effective_proxy);
    if (!ep || ep->is_socks()) return;
    if (ep->scheme != "http" && ep->scheme != "https") return;
    std::wstring wuser = to_wide(ep->user);
    std::wstring wpass = to_wide(ep->pass);
    if (!wuser.empty())
        WinHttpSetOption(request, WINHTTP_OPTION_PROXY_USERNAME, wuser.data(),
                         static_cast<DWORD>(wuser.size()));
    if (!wpass.empty())
        WinHttpSetOption(request, WINHTTP_OPTION_PROXY_PASSWORD, wpass.data(),
                         static_cast<DWORD>(wpass.size()));
}

}  // namespace

WsTransport::~WsTransport() { close(); }

bool WsTransport::connect(const std::string& host, std::uint16_t port, const std::string& path,
                          const std::string& effective_proxy, int handshake_timeout_ms) {
    close();
    session_ = make_session(effective_proxy);
    if (!session_) {
        SAM_LOG_WARN("cm: WinHttpOpen failed");
        return false;
    }
    conn_ = WinHttpConnect(static_cast<HINTERNET>(session_), to_wide(host).c_str(), port, 0);
    if (!conn_) {
        SAM_LOG_WARN("cm: WinHttpConnect failed for {}:{}", host, port);
        close();
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(static_cast<HINTERNET>(conn_), L"GET",
                                           to_wide(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        close();
        return false;
    }
    WinHttpSetTimeouts(request, handshake_timeout_ms, handshake_timeout_ms,
                       handshake_timeout_ms, handshake_timeout_ms);
    apply_proxy_creds(request, effective_proxy);
    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        WinHttpCloseHandle(request);
        close();
        return false;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        SAM_LOG_WARN("cm: websocket upgrade failed ({})", GetLastError());
        WinHttpCloseHandle(request);
        close();
        return false;
    }
    ws_ = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    if (!ws_) {
        SAM_LOG_WARN("cm: WinHttpWebSocketCompleteUpgrade failed ({})", GetLastError());
        close();
        return false;
    }
    // No receive timeout: the reader blocks until a message/close. Sends still time
    // out so a wedged write can't hang the worker.
    WinHttpSetTimeouts(static_cast<HINTERNET>(ws_), kNoTimeoutMs, kNoTimeoutMs, kSendTimeoutMs,
                       kNoTimeoutMs);
    {
        std::lock_guard lk(mtx_);
        queue_.clear();
        terminal_ = false;
    }
    closing_.store(false);
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

bool WsTransport::send_binary(const std::uint8_t* data, std::size_t len) {
    if (!ws_) return false;
    const DWORD rc = WinHttpWebSocketSend(static_cast<HINTERNET>(ws_),
                                          WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                          const_cast<std::uint8_t*>(data), static_cast<DWORD>(len));
    if (rc != NO_ERROR) {
        if (!closing_.load()) SAM_LOG_WARN("cm: websocket send failed ({})", rc);
        return false;
    }
    return true;
}

RecvStatus WsTransport::recv_one(std::vector<std::uint8_t>& out) {
    out.clear();
    std::uint8_t chunk[8192];
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD rc = WinHttpWebSocketReceive(static_cast<HINTERNET>(ws_), chunk,
                                                 sizeof(chunk), &read, &type);
        if (rc != NO_ERROR) {
            if (!closing_.load()) SAM_LOG_INFO("cm: websocket receive failed ({})", rc);
            return RecvStatus::Error;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return RecvStatus::Closed;
        out.insert(out.end(), chunk, chunk + read);
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            return RecvStatus::Message;
        // Fragment buffer types: keep reading until the message completes.
    }
}

void WsTransport::reader_loop() {
    for (;;) {
        std::vector<std::uint8_t> msg;
        const RecvStatus st = recv_one(msg);
        std::lock_guard lk(mtx_);
        if (st == RecvStatus::Message) {
            queue_.push_back(std::move(msg));
            cv_.notify_one();
            continue;
        }
        terminal_ = true;
        terminal_status_ = st;
        cv_.notify_one();
        return;
    }
}

RecvStatus WsTransport::next_message(std::vector<std::uint8_t>& out, int timeout_ms) {
    std::unique_lock lk(mtx_);
    if (queue_.empty() && !terminal_)
        cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [this] { return !queue_.empty() || terminal_; });
    if (!queue_.empty()) {
        out = std::move(queue_.front());
        queue_.pop_front();
        return RecvStatus::Message;
    }
    if (terminal_) return terminal_status_;
    return RecvStatus::Timeout;
}

void WsTransport::close() {
    closing_.store(true);
    if (ws_) {
        // Abortive close: unblocks the reader's pending receive immediately. The
        // app-level ClientLogOff has already been sent by CmSession::disconnect.
        WinHttpCloseHandle(static_cast<HINTERNET>(ws_));
    }
    if (reader_.joinable()) reader_.join();
    ws_ = nullptr;
    if (conn_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(conn_));
        conn_ = nullptr;
    }
    if (session_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(session_));
        session_ = nullptr;
    }
    {
        std::lock_guard lk(mtx_);
        queue_.clear();
        terminal_ = false;
    }
    closing_.store(false);
}

}  // namespace sam::steam_cm
