#include "core/cs2_gc/gc_message.hpp"

#include "core/cs2_gc/gc_emsg.hpp"
#include "core/steam_auth/gen/steammessages_base.pb.h"

namespace sam::cs2_gc {

namespace {

void put_u32(std::string& buf, std::uint32_t v) {
    buf.push_back(static_cast<char>(v & 0xff));
    buf.push_back(static_cast<char>((v >> 8) & 0xff));
    buf.push_back(static_cast<char>((v >> 16) & 0xff));
    buf.push_back(static_cast<char>((v >> 24) & 0xff));
}

std::uint32_t read_u32(const std::string& s, std::size_t off) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data()) + off;
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

void build_gc_client(::CMsgGCClient& out, std::uint32_t gc_emsg, const std::string& gc_body) {
    ::CMsgProtoBufHeader gc_header;
    const std::string header_bytes = gc_header.SerializeAsString();
    const std::uint32_t msgtype = gc_emsg | kGcProtoMask;

    std::string payload;
    payload.reserve(8 + header_bytes.size() + gc_body.size());
    put_u32(payload, msgtype);
    put_u32(payload, static_cast<std::uint32_t>(header_bytes.size()));
    payload += header_bytes;
    payload += gc_body;

    out.set_appid(kCs2AppId);
    out.set_msgtype(msgtype);
    out.set_payload(payload);
}

bool parse_gc_client(const ::CMsgGCClient& in, std::uint32_t& gc_emsg, std::string& gc_body) {
    const std::string& payload = in.payload();
    if (payload.size() < 8) return false;
    gc_emsg = read_u32(payload, 0) & ~kGcProtoMask;
    const std::uint32_t header_len = read_u32(payload, 4);
    if (8 + static_cast<std::size_t>(header_len) > payload.size()) return false;
    gc_body = payload.substr(8 + header_len);
    return true;
}

}  // namespace sam::cs2_gc
