#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sam::steam_cm {

enum class RecvStatus {
    Message,
    Timeout,
    Closed,
    Error,
};

// One CM WebSocket connection over WinHTTP. The blocking receive runs on a dedicated
// reader thread with no receive timeout (WinHTTP web sockets abort the connection
// when a receive times out, so polling with a short timeout silently killed the link
// the moment Steam went idle). The owning worker thread sends and drains received
// messages via next_message(); WinHTTP permits one concurrent send + one receive.
// It builds its own session from the caller-supplied effective proxy (see
// http::current_effective_proxy), inheriting per-account routing including the SOCKS5
// bridge.
class WsTransport {
public:
    WsTransport() = default;
    ~WsTransport();
    WsTransport(const WsTransport&) = delete;
    WsTransport& operator=(const WsTransport&) = delete;

    bool connect(const std::string& host, std::uint16_t port, const std::string& path,
                 const std::string& effective_proxy, int handshake_timeout_ms);
    bool send_binary(const std::uint8_t* data, std::size_t len);
    // Returns the next complete message the reader has queued, waiting up to
    // timeout_ms. Timeout means none arrived yet (the connection is still alive);
    // Closed/Error mean the socket went away. A short timeout never tears the socket
    // down. Buffered messages are drained before a terminal status is reported.
    RecvStatus next_message(std::vector<std::uint8_t>& out, int timeout_ms);
    void close();
    bool connected() const { return ws_ != nullptr; }

private:
    void reader_loop();
    RecvStatus recv_one(std::vector<std::uint8_t>& out);

    void* session_ = nullptr;  // HINTERNET
    void* conn_ = nullptr;     // HINTERNET
    void* ws_ = nullptr;       // HINTERNET, the upgraded web socket

    std::thread reader_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> queue_;
    bool terminal_ = false;
    RecvStatus terminal_status_ = RecvStatus::Error;
    std::atomic<bool> closing_{false};
};

}  // namespace sam::steam_cm
