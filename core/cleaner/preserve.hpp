#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sam::cleaner {

// The accounts a clean spares, and nothing else: there is deliberately no path or registry
// escape hatch and no "keep every ssfn" switch. Strict by design: an account not listed here
// loses its saved login, userdata, cached avatar and ConnectCache token.
struct PreserveList {
    std::vector<std::wstring> account_ids;  // SteamID64 as decimal strings

    bool preserves(std::wstring_view steamid64) const;
    bool empty() const noexcept { return account_ids.empty(); }
};

}  // namespace sam::cleaner
