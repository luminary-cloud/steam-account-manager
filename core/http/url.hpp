#pragma once

#include <map>
#include <string>
#include <string_view>

namespace sam::http {

std::string url_encode(std::string_view s);

// Leading `?` is included if `fields` is non-empty.
std::string make_query(const std::map<std::string, std::string>& fields);

std::string build_url(std::string_view base,
                      const std::map<std::string, std::string>& fields);

}  // namespace sam::http
