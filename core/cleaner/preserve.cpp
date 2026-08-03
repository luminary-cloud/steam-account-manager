#include "core/cleaner/preserve.hpp"

#include <algorithm>

namespace sam::cleaner {

bool PreserveList::preserves(std::wstring_view steamid64) const {
    if (steamid64.empty()) return false;
    return std::any_of(account_ids.begin(), account_ids.end(),
                       [&](const std::wstring& id) { return id == steamid64; });
}

}  // namespace sam::cleaner
