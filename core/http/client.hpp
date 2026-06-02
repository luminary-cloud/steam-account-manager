#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/http/response.hpp"

namespace sam::http {

enum class Method {
    Get,
    Post,
    Put,
    Delete,
};

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path = "/";
    bool secure = true;
    bool http_only = false;
};

struct Request {
    Method method = Method::Get;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::vector<Cookie> cookies;
    int timeout_seconds = 15;
    int connect_timeout_seconds = 6;
    std::string user_agent =
        "Mozilla/5.0 (Linux; U; Android 9; en-US; "
        "SM-G973U) Valve Steam App Version/3.6.4";
    bool follow_redirects = true;
    int max_retries = 2;
};

// Performs a single request. Retries on transport errors and 5xx responses up to
// `max_retries` with jittered exponential backoff. Never throws.
Response request(const Request& req);

// Aborts all in-flight requests and makes subsequent ones fail immediately.
// Call once at shutdown so blocked worker threads don't wait out the timeout.
void cancel_all();

// Encodes a flat map as application/x-www-form-urlencoded.
std::string form_encode(const std::map<std::string, std::string>& fields);

}  // namespace sam::http
