#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/account_store/account.hpp"

namespace sam::sda {

enum class ConfirmationType {
    Unknown = 0,
    Trade,
    MarketListing,
    PhoneNumberChange,
    AccountRecovery,
    FeatureOptOut,
};

struct Confirmation {
    std::string id;             // numeric id carried as a string
    std::string nonce;
    std::string creator_id;
    std::string headline;
    std::string summary;
    std::string icon_url;
    std::int64_t creation_unix = 0;
    ConfirmationType type = ConfirmationType::Unknown;
};

struct ConfirmationFetchResult {
    bool ok = false;
    int http_status = 0;
    std::string error;
    std::vector<Confirmation> items;
};

// Error sentinel set by respond_to_confirmation when Steam returns
// `{"success":false}` with no `message` field. The caller may want to refetch
// the list (the cid/ck may be stale) or trigger an auto-relogin (the cookie
// may be stale or the access_token may have drifted off the mobile audience).
inline constexpr char kRespondSuccessFalse[] = "steam: success=false";

ConfirmationFetchResult fetch_confirmations(const core::Account& account);

bool respond_to_confirmation(const core::Account& account,
                              const Confirmation& c,
                              bool allow,
                              std::string* error_out = nullptr);

// Bulk version using /mobileconf/multiajaxop.
bool respond_bulk(const core::Account& account,
                   const std::vector<Confirmation>& items,
                   bool allow,
                   std::string* error_out = nullptr);

}  // namespace sam::sda
