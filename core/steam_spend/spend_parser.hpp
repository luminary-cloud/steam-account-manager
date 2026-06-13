#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sam::steam_spend {

// Parsed help.steampowered.com AccountSpend page. TotalSpend is total external
// funds applied to the account.
struct SpendData {
    bool ok = false;                       // body parsed as a real AccountSpend page
    bool found_total = false;
    bool currency_is_usd = false;
    std::int64_t total_spend_cents = -1;   // -1 = not found / unparseable
    std::string currency;                  // raw currency code, e.g. "USD" / "RMB"
};

SpendData parse_account_spend(std::string_view html);

// Content-signature checks distinguishing a real AccountSpend response from the
// login page Steam serves for an invalid session cookie.
bool looks_like_spend_page(std::string_view html);
bool looks_like_login_page(std::string_view html);

}  // namespace sam::steam_spend
