#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/account_store/account.hpp"

namespace sam::core {

// Order matters: the value is persisted as an int (settings.accounts_sort) and indexes
// the sort dropdown's labels in accounts_screen.cpp. Append new keys; never reorder.
enum class SortKey {
    PersonaAsc,
    PersonaDesc,
    LoginAsc,
    LastLoginDesc,
    CreatedDesc,
    PremierDesc,
    SteamLevelDesc,
    Cs2LevelDesc,
    NoCooldownFirst,
    DropUnclaimedFirst,
    Cs2XpDesc,
    TotalSpendDesc,
    CooldownSoonest,
};

struct AccountFilter {
    std::string query;
    std::vector<std::string> required_tag_ids;
    bool only_with_vac = false;
    bool only_with_sda = false;
    bool only_with_cooldown = false;        // only ACTIVE cooldowns
    bool only_with_any_ban = false;         // any of vac/game/community/trade
    bool only_with_prime = false;
    bool exclude_red = false;
    TrustLabel trust_filter = TrustLabel::Unset;  // Unset means no trust filter
    SortKey sort = SortKey::PersonaAsc;
    std::int64_t now_unix = 0;              // for time-relative predicates
};

// Indices into `accounts` that match `filter`, in sorted order.
std::vector<std::size_t> apply_filter(const std::vector<Account>& accounts,
                                      const AccountFilter& filter);

}  // namespace sam::core
