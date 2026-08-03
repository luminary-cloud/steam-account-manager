#include "core/steam_cm/multi.hpp"

#include "core/log.hpp"

extern "C" {
#include "puff.h"
}

namespace sam::steam_cm {

namespace {

bool gunzip(const std::string& in, std::uint32_t out_size, std::string& out) {
    if (in.size() < 18) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    if (p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return false;
    const unsigned char flags = p[3];
    std::size_t off = 10;
    if (flags & 0x04) {
        if (off + 2 > in.size()) return false;
        const std::size_t xlen = static_cast<std::size_t>(p[off]) | (static_cast<std::size_t>(p[off + 1]) << 8);
        off += 2 + xlen;
    }
    if (flags & 0x08) {
        while (off < in.size() && p[off] != 0) ++off;
        ++off;
    }
    if (flags & 0x10) {
        while (off < in.size() && p[off] != 0) ++off;
        ++off;
    }
    if (flags & 0x02) off += 2;
    if (off + 8 > in.size()) return false;

    out.resize(out_size);
    unsigned long destlen = out_size;
    unsigned long sourcelen = static_cast<unsigned long>(in.size() - off - 8);
    const int rc = puff(out_size ? reinterpret_cast<unsigned char*>(out.data()) : nullptr,
                        &destlen, p + off, &sourcelen);
    if (rc != 0 || destlen != out_size) {
        SAM_LOG_WARN("cm: gzip inflate failed (rc={}, {} of {} bytes)", rc, destlen, out_size);
        return false;
    }
    return true;
}

bool split_frames(const std::string& blob, std::vector<std::vector<std::uint8_t>>& out) {
    std::size_t off = 0;
    while (off + 4 <= blob.size()) {
        const auto* p = reinterpret_cast<const unsigned char*>(blob.data() + off);
        const std::uint32_t len = static_cast<std::uint32_t>(p[0]) |
                                  (static_cast<std::uint32_t>(p[1]) << 8) |
                                  (static_cast<std::uint32_t>(p[2]) << 16) |
                                  (static_cast<std::uint32_t>(p[3]) << 24);
        off += 4;
        if (off + len > blob.size()) return false;
        out.emplace_back(blob.begin() + off, blob.begin() + off + len);
        off += len;
    }
    return off == blob.size();
}

}  // namespace

bool expand_multi(const std::string& payload, std::uint32_t size_unzipped,
                  std::vector<std::vector<std::uint8_t>>& out) {
    if (size_unzipped == 0) return split_frames(payload, out);
    std::string raw;
    if (!gunzip(payload, size_unzipped, raw)) return false;
    return split_frames(raw, out);
}

}  // namespace sam::steam_cm
