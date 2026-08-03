#include "core/steam_cm/cm_session.hpp"

#include <random>
#include <vector>

#include "core/http/client.hpp"
#include "core/log.hpp"
#include "core/steam_auth/gen/steammessages_clientserver_extra.pb.h"
#include "core/steam_auth/gen/steammessages_clientserver_login.pb.h"
#include "core/steam_cm/cm_server_list.hpp"
#include "core/steam_cm/emsg.hpp"
#include "core/steam_cm/message.hpp"
#include "core/steam_cm/multi.hpp"

namespace sam::steam_cm {

namespace {
constexpr int kEResultOK = 1;
constexpr std::uint32_t kProtocolVersion = 65580;
constexpr int kOsTypeWindows = 16;

std::uint32_t new_logon_id() {
    std::random_device rd;
    return static_cast<std::uint32_t>(rd()) | 1u;
}
}  // namespace

bool CmSession::connect(const std::string& refresh_token, std::uint64_t steam_id,
                        std::string& error) {
    if (refresh_token.empty()) {
        error = "no refresh token";
        return false;
    }
    const auto servers = fetch_cm_list();
    if (servers.empty()) {
        error = "no CM servers available";
        return false;
    }
    const std::string proxy = http::current_effective_proxy();

    const int tries = std::min<int>(3, static_cast<int>(servers.size()));
    for (int i = 0; i < tries; ++i) {
        if (aborted()) { error = "cancelled"; return false; }
        const auto& srv = servers[i];
        SAM_LOG_INFO("cm: connecting to {}:{}", srv.host, srv.port);
        if (!ws_.connect(srv.host, srv.port, "/cmsocket/", proxy, 15000)) {
            error = "websocket connect failed";
            continue;
        }
        connected_ = true;
        logged_on_ = false;
        logon_done_ = false;
        logon_eresult_ = 0;
        playing_blocked_ = false;
        playing_state_seen_ = false;
        playing_app_ = 0;
        session_id_ = 0;
        steam_id_ = steam_id;
        last_heartbeat_ = std::chrono::steady_clock::now();

        CMsgClientLogon logon;
        logon.set_protocol_version(kProtocolVersion);
        logon.set_client_os_type(kOsTypeWindows);
        logon.set_should_remember_password(true);
        logon.set_supports_rate_limit_response(true);
        logon.set_client_language("english");
        logon.set_access_token(refresh_token);
        logon.mutable_obfuscated_private_ip()->set_v4(new_logon_id());
        if (!send(EMsg::ClientLogon, logon)) {
            ws_.close();
            connected_ = false;
            continue;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        const Handler noop = [](std::uint32_t, const ::CMsgProtoBufHeader&, const std::string&) {};
        while (!logon_done_ && !aborted() && std::chrono::steady_clock::now() < deadline) {
            if (!pump(500, noop)) break;
        }

        if (logged_on_) {
            SAM_LOG_INFO("cm: logged on steamid={} session={} heartbeat={}s",
                         steam_id_, session_id_, heartbeat_seconds_);
            return true;
        }
        ws_.close();
        connected_ = false;
        if (logon_done_) {
            error = "logon rejected (eresult " + std::to_string(logon_eresult_) + ")";
            return false;
        }
        error = "logon timed out";
    }
    if (error.empty()) error = "could not connect to any CM";
    return false;
}

bool CmSession::send(std::uint32_t emsg, const google::protobuf::MessageLite& body) {
    return send_raw(emsg, body.SerializeAsString());
}

bool CmSession::send_raw(std::uint32_t emsg, const std::string& body) {
    ::CMsgProtoBufHeader header;
    return send_with_header(emsg, header, body);
}

bool CmSession::send_service_method(std::string_view target_job_name,
                                    const google::protobuf::MessageLite& body,
                                    std::uint64_t jobid) {
    ::CMsgProtoBufHeader header;
    header.set_target_job_name(std::string(target_job_name));
    header.set_jobid_source(jobid);
    return send_with_header(EMsg::ServiceMethodCallFromClient, header,
                            body.SerializeAsString());
}

bool CmSession::send_with_header(std::uint32_t emsg, ::CMsgProtoBufHeader& header,
                                 const std::string& body) {
    if (steam_id_ != 0) header.set_steamid(steam_id_);
    header.set_client_sessionid(session_id_);
    const auto frame = encode(emsg, header, body);
    return ws_.send_binary(frame.data(), frame.size());
}

bool CmSession::pump(int timeout_ms, const Handler& handler) {
    if (aborted()) return false;
    maybe_heartbeat();
    std::vector<std::uint8_t> buf;
    const RecvStatus st = ws_.next_message(buf, timeout_ms);
    if (st == RecvStatus::Timeout) return connected_;
    if (st == RecvStatus::Closed || st == RecvStatus::Error) {
        connected_ = false;
        logged_on_ = false;
        return false;
    }
    process_frame(buf.data(), buf.size(), handler);
    return connected_;
}

void CmSession::process_frame(const std::uint8_t* data, std::size_t len, const Handler& handler) {
    DecodedMessage msg;
    if (!decode(data, len, msg)) {
        SAM_LOG_DEBUG("cm: dropped undecodable frame ({} bytes)", len);
        return;
    }
    process_message(msg.emsg, msg.header, msg.body, handler);
}

void CmSession::process_message(std::uint32_t emsg, const ::CMsgProtoBufHeader& header,
                                const std::string& body, const Handler& handler) {
    switch (emsg) {
        case EMsg::Multi: {
            CMsgMulti multi;
            if (!multi.ParseFromString(body)) return;
            std::vector<std::vector<std::uint8_t>> frames;
            if (!expand_multi(multi.message_body(), multi.size_unzipped(), frames)) {
                SAM_LOG_WARN("cm: failed to expand multi ({} unzipped)", multi.size_unzipped());
                return;
            }
            for (const auto& f : frames) {
                DecodedMessage sub;
                if (decode(f.data(), f.size(), sub))
                    process_message(sub.emsg, sub.header, sub.body, handler);
            }
            return;
        }
        case EMsg::ClientLogOnResponse: {
            CMsgClientLogonResponse resp;
            resp.ParseFromString(body);
            logon_done_ = true;
            logon_eresult_ = resp.eresult();
            if (resp.eresult() == kEResultOK) {
                logged_on_ = true;
                if (header.has_steamid()) steam_id_ = header.steamid();
                if (header.has_client_sessionid()) session_id_ = header.client_sessionid();
                heartbeat_seconds_ = resp.heartbeat_seconds() > 0 ? resp.heartbeat_seconds() : 9;
                last_heartbeat_ = std::chrono::steady_clock::now();
            }
            return;
        }
        case EMsg::ClientLoggedOff: {
            CMsgClientLoggedOff off;
            off.ParseFromString(body);
            SAM_LOG_INFO("cm: logged off (eresult {})", off.eresult());
            logoff_eresult_ = off.eresult();
            logged_on_ = false;
            connected_ = false;
            return;
        }
        case EMsg::ClientHeartBeat:
            return;
        case EMsg::ClientPlayingSessionState: {
            CMsgClientPlayingSessionState st;
            if (st.ParseFromString(body)) {
                playing_blocked_ = st.playing_blocked();
                playing_app_ = st.playing_app();
                playing_state_seen_ = true;
                SAM_LOG_INFO("cm: playing-session state blocked={} app={}", playing_blocked_, playing_app_);
            }
            return;
        }
        default:
            if (handler) handler(emsg, header, body);
            return;
    }
}

void CmSession::maybe_heartbeat() {
    if (!logged_on_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat_ < std::chrono::seconds(heartbeat_seconds_)) return;
    CMsgClientHeartBeat hb;
    send(EMsg::ClientHeartBeat, hb);
    last_heartbeat_ = now;
}

void CmSession::kick_playing_session() {
    CMsgClientKickPlayingSession kick;
    send(EMsg::ClientKickPlayingSession, kick);
}

void CmSession::disconnect() {
    if (connected_ && logged_on_) {
        CMsgClientLogOff off;
        send(EMsg::ClientLogOff, off);
    }
    ws_.close();
    connected_ = false;
    logged_on_ = false;
}

}  // namespace sam::steam_cm
