#include "core/steam_cm/message.hpp"

#include "core/steam_cm/emsg.hpp"

namespace sam::steam_cm {

namespace {

void put_u32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xff));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::vector<std::uint8_t> encode(std::uint32_t emsg, const ::CMsgProtoBufHeader& header,
                                 const std::string& body) {
    const std::string header_bytes = header.SerializeAsString();
    std::vector<std::uint8_t> out;
    out.reserve(8 + header_bytes.size() + body.size());
    put_u32(out, emsg | kProtoMask);
    put_u32(out, static_cast<std::uint32_t>(header_bytes.size()));
    out.insert(out.end(), header_bytes.begin(), header_bytes.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool decode(const std::uint8_t* data, std::size_t len, DecodedMessage& out) {
    if (len < 8) return false;
    const std::uint32_t raw = read_u32(data);
    if (!(raw & kProtoMask)) return false;
    out.emsg = raw & ~kProtoMask;
    const std::uint32_t header_len = read_u32(data + 4);
    if (8 + static_cast<std::size_t>(header_len) > len) return false;
    if (!out.header.ParseFromArray(data + 8, static_cast<int>(header_len))) return false;
    const std::size_t body_off = 8 + header_len;
    out.body.assign(reinterpret_cast<const char*>(data + body_off), len - body_off);
    return true;
}

}  // namespace sam::steam_cm
