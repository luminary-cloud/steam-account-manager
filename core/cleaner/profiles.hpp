#pragma once

#include <span>
#include <string>
#include <vector>

namespace sam::cleaner {

// A named bundle of target ids. Profiles are the only granularity the UI offers (there is no
// per-target picker), so the three below have to stand on their own.
struct Profile {
    std::wstring name;
    std::wstring description;
    std::vector<std::string> target_ids;
    bool requires_confirmation = false;  // Full Wipe
};

// Ordered to match app::CleanerProfile: [0] Quick Clean, [1] Account Reset, [2] Full Wipe.
// Stable across calls in the same process.
std::span<const Profile> built_in_profiles();

}  // namespace sam::cleaner
