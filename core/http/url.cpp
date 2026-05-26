#include "core/http/url.hpp"

namespace sam::http {

namespace {

constexpr char kHex[] = "0123456789ABCDEF";

bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

}  // namespace

std::string url_encode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (is_unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string make_query(const std::map<std::string, std::string>& fields) {
    if (fields.empty()) return {};
    std::string out = "?";
    bool first = true;
    for (const auto& [k, v] : fields) {
        if (!first) out += '&';
        first = false;
        out += url_encode(k);
        out += '=';
        out += url_encode(v);
    }
    return out;
}

std::string build_url(std::string_view base,
                      const std::map<std::string, std::string>& fields) {
    std::string out(base);
    out += make_query(fields);
    return out;
}

}  // namespace sam::http
