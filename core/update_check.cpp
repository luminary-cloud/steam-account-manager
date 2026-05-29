#include "core/update_check.hpp"

#include <array>
#include <charconv>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/http/client.hpp"
#include "core/version.hpp"

namespace sam::core::update_check {
namespace {

std::optional<std::array<int, 3>> parse_semver(std::string_view s) {
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) {
        s.remove_prefix(1);
    }
    std::array<int, 3> out{0, 0, 0};
    std::size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        std::size_t start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            ++pos;
        }
        if (start == pos) {
            return std::nullopt;
        }
        int v = 0;
        auto [_, ec] = std::from_chars(s.data() + start, s.data() + pos, v);
        if (ec != std::errc{}) {
            return std::nullopt;
        }
        out[i] = v;
        if (i < 2) {
            if (pos >= s.size() || s[pos] != '.') {
                return std::nullopt;
            }
            ++pos;
        }
    }
    return out;
}

}  // namespace

bool semver_less(std::string_view lhs, std::string_view rhs) {
    auto a = parse_semver(lhs);
    auto b = parse_semver(rhs);
    if (!a || !b) {
        return false;
    }
    return *a < *b;
}

std::optional<Result> fetch_latest_release(std::string_view owner_repo,
                                           std::string_view current_version) {
    http::Request req;
    req.method = http::Method::Get;
    req.url = "https://api.github.com/repos/" + std::string(owner_repo) + "/releases/latest";
    req.headers["Accept"] = "application/vnd.github+json";
    req.headers["X-GitHub-Api-Version"] = "2022-11-28";
    req.user_agent = "steam-account-manager/" + std::string(sam::kVersion);
    req.timeout_seconds = 5;
    req.connect_timeout_seconds = 3;
    req.max_retries = 0;

    const http::Response resp = http::request(req);
    if (resp.transport_error || resp.status != 200) {
        spdlog::warn("update_check: request failed (status={}, transport_error={})",
                     resp.status, resp.transport_error);
        return std::nullopt;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(resp.body);
    } catch (const std::exception& e) {
        spdlog::warn("update_check: JSON parse failed: {}", e.what());
        return std::nullopt;
    }

    auto it = j.find("tag_name");
    if (it == j.end() || !it->is_string()) {
        spdlog::warn("update_check: response missing tag_name");
        return std::nullopt;
    }

    Result out;
    out.latest_tag = it->get<std::string>();
    out.newer_than_current = semver_less(current_version, out.latest_tag);
    spdlog::info("update_check: latest={} current={} newer={}", out.latest_tag,
                 std::string(current_version), out.newer_than_current);
    return out;
}

}  // namespace sam::core::update_check
