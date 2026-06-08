#include "core/http/proxy.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

#include "core/log.hpp"

namespace sam::http {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

void ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

bool send_all(SOCKET s, const unsigned char* data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = send(s, reinterpret_cast<const char*>(data) + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool recv_n(SOCKET s, unsigned char* buf, int n) {
    int got = 0;
    while (got < n) {
        const int r = recv(s, reinterpret_cast<char*>(buf) + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

void set_io_timeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

// Opens a TCP connection to host:port with a bounded connect time. Returns
// INVALID_SOCKET on failure.
SOCKET dial(const std::string& host, std::uint16_t port, int timeout_sec) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        const int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = rc == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv{timeout_sec, 0};
            if (select(0, nullptr, &wf, nullptr, &tv) > 0 && FD_ISSET(s, &wf)) {
                int err = 0;
                int len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                ok = err == 0;
            }
        }
        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        if (ok) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

// Performs the SOCKS5 greeting, optional username/password auth (RFC 1929), and a
// CONNECT to host:port (sent as a domain name so the proxy resolves it). Returns
// true once the upstream reports the tunnel is open.
bool socks5_connect(SOCKET up, const ProxyEndpoint& px,
                    const std::string& host, std::uint16_t port) {
    const bool have_creds = !px.user.empty() || !px.pass.empty();

    unsigned char greet[4];
    int glen;
    if (have_creds) {
        greet[0] = 0x05; greet[1] = 0x02; greet[2] = 0x00; greet[3] = 0x02; glen = 4;
    } else {
        greet[0] = 0x05; greet[1] = 0x01; greet[2] = 0x00; glen = 3;
    }
    if (!send_all(up, greet, glen)) return false;

    unsigned char sel[2];
    if (!recv_n(up, sel, 2) || sel[0] != 0x05) return false;
    if (sel[1] == 0x02) {
        if (px.user.size() > 255 || px.pass.size() > 255) return false;
        std::vector<unsigned char> a;
        a.push_back(0x01);
        a.push_back(static_cast<unsigned char>(px.user.size()));
        a.insert(a.end(), px.user.begin(), px.user.end());
        a.push_back(static_cast<unsigned char>(px.pass.size()));
        a.insert(a.end(), px.pass.begin(), px.pass.end());
        if (!send_all(up, a.data(), static_cast<int>(a.size()))) return false;
        unsigned char ar[2];
        if (!recv_n(up, ar, 2) || ar[1] != 0x00) return false;
    } else if (sel[1] != 0x00) {
        return false;  // 0xFF (no acceptable methods) or an unexpected method
    }

    if (host.size() > 255) return false;
    std::vector<unsigned char> req;
    req.push_back(0x05);  // version
    req.push_back(0x01);  // CONNECT
    req.push_back(0x00);  // reserved
    req.push_back(0x03);  // address type: domain name
    req.push_back(static_cast<unsigned char>(host.size()));
    req.insert(req.end(), host.begin(), host.end());
    req.push_back(static_cast<unsigned char>(port >> 8));
    req.push_back(static_cast<unsigned char>(port & 0xFF));
    if (!send_all(up, req.data(), static_cast<int>(req.size()))) return false;

    unsigned char head[4];
    if (!recv_n(up, head, 4) || head[0] != 0x05 || head[1] != 0x00) return false;
    int skip = 0;
    switch (head[3]) {
        case 0x01: skip = 4; break;
        case 0x04: skip = 16; break;
        case 0x03: {
            unsigned char l = 0;
            if (!recv_n(up, &l, 1)) return false;
            skip = l;
            break;
        }
        default: return false;
    }
    unsigned char dump[256];
    if (skip > 0 && !recv_n(up, dump, skip)) return false;
    unsigned char bound_port[2];
    return recv_n(up, bound_port, 2);
}

// Reads the "CONNECT host:port HTTP/1.1" request WinHTTP sends, one byte at a time
// so we never swallow tunnel bytes that follow the blank line.
bool read_connect_target(SOCKET client, std::string& host, std::uint16_t& port) {
    std::string buf;
    unsigned char c = 0;
    while (buf.find("\r\n\r\n") == std::string::npos) {
        if (!recv_n(client, &c, 1)) return false;
        buf.push_back(static_cast<char>(c));
        if (buf.size() > 16384) return false;
    }
    const auto line_end = buf.find("\r\n");
    const std::string line = buf.substr(0, line_end);
    const auto sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    const auto sp2 = line.find(' ', sp1 + 1);
    const std::string method = line.substr(0, sp1);
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (method != "CONNECT") return false;
    const auto colon = target.rfind(':');
    if (colon == std::string::npos) return false;
    host = target.substr(0, colon);
    int p = 0;
    try {
        p = std::stoi(target.substr(colon + 1));
    } catch (...) {
        return false;
    }
    if (host.empty() || p <= 0 || p > 65535) return false;
    port = static_cast<std::uint16_t>(p);
    return true;
}

void relay(SOCKET from, SOCKET to) {
    char buf[16384];
    for (;;) {
        const int r = recv(from, buf, sizeof(buf), 0);
        if (r <= 0) break;
        if (!send_all(to, reinterpret_cast<unsigned char*>(buf), r)) break;
    }
}

void send_status(SOCKET client, const char* status_line) {
    std::string r = status_line;
    r += "\r\nConnection: close\r\n\r\n";
    send_all(client, reinterpret_cast<const unsigned char*>(r.data()), static_cast<int>(r.size()));
}

void handle_connection(SOCKET client, ProxyEndpoint upstream) {
    set_io_timeout(client, 20000);
    std::string host;
    std::uint16_t port = 0;
    if (!read_connect_target(client, host, port)) {
        send_status(client, "HTTP/1.1 400 Bad Request");
        closesocket(client);
        return;
    }

    SOCKET up = dial(upstream.host, upstream.port, 10);
    if (up == INVALID_SOCKET) {
        SAM_LOG_WARN("proxy: cannot reach socks5 upstream {}:{}", upstream.host, upstream.port);
        send_status(client, "HTTP/1.1 502 Bad Gateway");
        closesocket(client);
        return;
    }
    set_io_timeout(up, 20000);
    if (!socks5_connect(up, upstream, host, port)) {
        SAM_LOG_WARN("proxy: socks5 CONNECT to {}:{} via {}:{} failed",
                     host, port, upstream.host, upstream.port);
        send_status(client, "HTTP/1.1 502 Bad Gateway");
        closesocket(up);
        closesocket(client);
        return;
    }

    const char* ok = "HTTP/1.1 200 Connection established\r\n\r\n";
    if (!send_all(client, reinterpret_cast<const unsigned char*>(ok), static_cast<int>(std::strlen(ok)))) {
        closesocket(up);
        closesocket(client);
        return;
    }

    // Tunnel is live; let both directions run untimed until either side closes.
    set_io_timeout(client, 0);
    set_io_timeout(up, 0);
    std::thread pump([client, up] {
        relay(client, up);
        shutdown(up, SD_SEND);
    });
    relay(up, client);
    shutdown(client, SD_SEND);
    pump.join();
    closesocket(up);
    closesocket(client);
}

void accept_loop(SOCKET listener, ProxyEndpoint upstream) {
    for (;;) {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        std::thread(handle_connection, client, upstream).detach();
    }
    closesocket(listener);
}

std::mutex g_mtx;
std::unordered_map<std::string, std::uint16_t> g_ports;

std::string upstream_key(const ProxyEndpoint& p) {
    return p.host + ":" + std::to_string(p.port) + "|" + p.user + "|" + p.pass;
}

}  // namespace

std::optional<ProxyEndpoint> parse_proxy(const std::string& raw) {
    const std::string url = trim(raw);
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return std::nullopt;

    ProxyEndpoint p;
    p.scheme = url.substr(0, scheme_end);
    std::transform(p.scheme.begin(), p.scheme.end(), p.scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string rest = url.substr(scheme_end + 3);
    if (const auto slash = rest.find('/'); slash != std::string::npos) {
        rest = rest.substr(0, slash);
    }

    std::string hostport = rest;
    if (const auto at = rest.rfind('@'); at != std::string::npos) {
        const std::string userinfo = rest.substr(0, at);
        hostport = rest.substr(at + 1);
        if (const auto colon = userinfo.find(':'); colon != std::string::npos) {
            p.user = userinfo.substr(0, colon);
            p.pass = userinfo.substr(colon + 1);
        } else {
            p.user = userinfo;
        }
    }

    const auto colon = hostport.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    p.host = hostport.substr(0, colon);
    if (p.scheme.empty() || p.host.empty()) return std::nullopt;
    int port = 0;
    try {
        port = std::stoi(hostport.substr(colon + 1));
    } catch (...) {
        return std::nullopt;
    }
    if (port <= 0 || port > 65535) return std::nullopt;
    p.port = static_cast<std::uint16_t>(port);
    return p;
}

std::optional<std::string> socks_bridge_address(const ProxyEndpoint& upstream) {
    ensure_winsock();
    const std::string key = upstream_key(upstream);

    std::lock_guard lk(g_mtx);
    if (const auto it = g_ports.find(key); it != g_ports.end()) {
        return "127.0.0.1:" + std::to_string(it->second);
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return std::nullopt;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // let the OS pick a free port
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listener);
        return std::nullopt;
    }
    sockaddr_in bound{};
    int blen = sizeof(bound);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &blen) == SOCKET_ERROR) {
        closesocket(listener);
        return std::nullopt;
    }
    const std::uint16_t port = ntohs(bound.sin_port);
    g_ports[key] = port;
    std::thread(accept_loop, listener, upstream).detach();
    SAM_LOG_DEBUG("proxy: socks5 bridge for {}:{} listening on 127.0.0.1:{}",
                  upstream.host, upstream.port, port);
    return "127.0.0.1:" + std::to_string(port);
}

}  // namespace sam::http
