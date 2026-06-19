#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "core/steam_auth/gen/steammessages_base.pb.h"
#include "core/steam_cm/ws_transport.hpp"

namespace google::protobuf {
class MessageLite;
}

namespace sam::steam_cm {

// A single authenticated CM connection over a WebSocket. Driven synchronously by
// its owning worker thread: connect() logs on, then the caller alternates
// send()/pump() to exchange messages while pump() keeps the heartbeat alive.
class CmSession {
public:
    using Handler = std::function<void(std::uint32_t emsg, const ::CMsgProtoBufHeader& header,
                                       const std::string& body)>;

    // `abort` (optional) lets the owner interrupt connect()/pump() loops on teardown.
    explicit CmSession(const std::atomic<bool>* abort = nullptr) : abort_(abort) {}

    // Picks a CM, opens the socket and logs on with the client-scoped refresh token.
    // Blocks until the logon response or a timeout. Sets `error` and returns false on
    // failure. `steam_id` seeds the logon header.
    bool connect(const std::string& refresh_token, std::uint64_t steam_id, std::string& error);

    // Sends a protobuf message, filling steamid/client_sessionid into the header.
    bool send(std::uint32_t emsg, const google::protobuf::MessageLite& body);
    bool send_raw(std::uint32_t emsg, const std::string& body);

    // Receives and dispatches for up to timeout_ms, expanding Multis and sending a
    // heartbeat when due. Returns false once the connection drops.
    bool pump(int timeout_ms, const Handler& handler);

    void disconnect();
    std::uint64_t steam_id() const { return steam_id_; }
    bool logged_on() const { return logged_on_; }
    // EResult from the last ClientLoggedOff (6 = LoggedInElsewhere), 0 if none.
    int last_logoff_eresult() const { return logoff_eresult_; }
    // True while Steam reports this account is playing a game on another session.
    bool playing_blocked() const { return playing_blocked_; }
    // True once the first ClientPlayingSessionState has arrived, so the GC launch can
    // stop waiting as soon as Steam reports the state instead of a fixed delay.
    bool playing_state_seen() const { return playing_state_seen_; }
    // Sends the take-over message that kicks the other session's game (the Steam
    // client's "start anyway" action).
    void kick_playing_session();

private:
    void process_frame(const std::uint8_t* data, std::size_t len, const Handler& handler);
    void process_message(std::uint32_t emsg, const ::CMsgProtoBufHeader& header,
                         const std::string& body, const Handler& handler);
    void maybe_heartbeat();
    bool aborted() const { return abort_ != nullptr && abort_->load(std::memory_order_relaxed); }

    const std::atomic<bool>* abort_ = nullptr;
    WsTransport ws_;
    std::int32_t session_id_ = 0;
    std::uint64_t steam_id_ = 0;
    int heartbeat_seconds_ = 9;
    bool connected_ = false;
    bool logged_on_ = false;
    bool logon_done_ = false;
    int logon_eresult_ = 0;
    int logoff_eresult_ = 0;
    bool playing_blocked_ = false;
    bool playing_state_seen_ = false;
    std::uint32_t playing_app_ = 0;
    std::chrono::steady_clock::time_point last_heartbeat_{};
};

}  // namespace sam::steam_cm
