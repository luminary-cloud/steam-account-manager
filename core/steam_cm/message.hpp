#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/steam_auth/gen/steammessages_base.pb.h"

namespace sam::steam_cm {

struct DecodedMessage {
    std::uint32_t emsg = 0;  // EMsg with the proto mask cleared
    ::CMsgProtoBufHeader header;
    std::string body;  // serialized message body that follows the header
};

// Frames a protobuf message the way a CM expects it: little-endian EMsg with the
// proto bit set, little-endian header length, the header, then the body.
std::vector<std::uint8_t> encode(std::uint32_t emsg, const ::CMsgProtoBufHeader& header,
                                 const std::string& body);

// Parses a single CM frame. Returns false if it is too short or not a protobuf
// message.
bool decode(const std::uint8_t* data, std::size_t len, DecodedMessage& out);

}  // namespace sam::steam_cm
