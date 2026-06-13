#pragma once

#include <map>
#include <string>
#include <vector>

namespace sam::http {

struct Response {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    // One per cookie; multiple Set-Cookie headers can't share a key in `headers`.
    std::vector<std::string> set_cookies;
    bool transport_error = false;  // WinHTTP-level failure (DNS, TLS, timeout)
    std::string error_message;
};

}  // namespace sam::http
