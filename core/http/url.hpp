#pragma once

#include <map>
#include <string>
#include <string_view>

namespace sam::http {

// Percent-encodes one URL component (path segment or query value).
std::string url_encode(std::string_view s);

// Builds a `?a=b&c=d` query string. Leading `?` is included if `fields` is non-empty.
std::string make_query(const std::map<std::string, std::string>& fields);

// Combines a base URL and a query map.
std::string build_url(std::string_view base,
                      const std::map<std::string, std::string>& fields);

}  // namespace sam::http
