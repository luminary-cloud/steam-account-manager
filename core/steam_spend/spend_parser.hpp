#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sam::steam_spend {

// Parsed result of the help.steampowered.com AccountSpend page. TotalSpend is
// the total external funds applied to the account; Steam reports it in USD.
struct SpendData {
    bool ok = false;                       // body parsed as a real AccountSpend page
    bool found_total = false;              // a TotalSpend row was located
    bool currency_is_usd = false;          // the TotalSpend row's currency cell == "USD"
    std::int64_t total_spend_cents = -1;   // -1 = not found / unparseable
    std::string currency;                  // raw currency code, e.g. "USD" / "RMB"
};

SpendData parse_account_spend(std::string_view html);

// Quick content-signature checks, mirroring the gcpd_parser equivalents, to
// distinguish a real AccountSpend response from the login page Steam serves
// when the session cookie is invalid.
bool looks_like_spend_page(std::string_view html);   // AccountSpend HTML detected
bool looks_like_login_page(std::string_view html);   // session expired -> needs re-login

}  // namespace sam::steam_spend
