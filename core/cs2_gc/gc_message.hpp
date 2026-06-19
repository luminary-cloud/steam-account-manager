#pragma once

#include <cstdint>
#include <string>

#include "core/steam_auth/gen/steammessages_clientserver_extra.pb.h"

namespace sam::cs2_gc {

// Wraps a serialized GC message body into a CMsgGCClient for sending inside a Steam
// ClientToGC message: payload = [u32 gc_emsg|proto][u32 header_len][header][body].
void build_gc_client(::CMsgGCClient& out, std::uint32_t gc_emsg, const std::string& gc_body);

// Splits a received CMsgGCClient (carried by ClientFromGC) back into the inner GC
// message id and its body. Returns false on a malformed payload.
bool parse_gc_client(const ::CMsgGCClient& in, std::uint32_t& gc_emsg, std::string& gc_body);

}  // namespace sam::cs2_gc
