#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace sam::core::update_check {

struct Result {
    std::string latest_tag;
    bool newer_than_current = false;
};

// Hits GitHub's /repos/<owner_repo>/releases/latest. Returns nullopt on any failure
// (offline, non-200, malformed JSON). owner_repo is the path part, e.g.
// "luminary-cloud/steam-account-manager".
std::optional<Result> fetch_latest_release(std::string_view owner_repo,
                                           std::string_view current_version);

// Strips a leading 'v', compares dotted integer segments. Returns false on any parse
// failure so a malformed tag never trips an update prompt.
bool semver_less(std::string_view lhs, std::string_view rhs);

}  // namespace sam::core::update_check
