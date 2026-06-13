#include "core/account_store/filter.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "core/strings.hpp"

namespace sam::core {

namespace {

std::string wstr_to_utf8_lower(const std::wstring& ws) {
    if (ws.empty()) return {};
    std::string out;
    out.reserve(ws.size());
    for (wchar_t c : ws) {
        if (c < 128) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

bool matches_query(const Account& a, const std::string& q_lower) {
    if (q_lower.empty()) return true;
    if (to_lower(a.login).find(q_lower) != std::string::npos) return true;
    if (to_lower(a.web.persona_name).find(q_lower) != std::string::npos) return true;
    if (wstr_to_utf8_lower(a.display_name).find(q_lower) != std::string::npos) return true;
    if (wstr_to_utf8_lower(a.notes).find(q_lower) != std::string::npos) return true;
    return false;
}

bool has_all_tags(const Account& a, const std::vector<std::string>& required) {
    for (const auto& need : required) {
        if (std::find(a.tag_ids.begin(), a.tag_ids.end(), need) == a.tag_ids.end()) {
            return false;
        }
    }
    return true;
}

bool any_ban(const Account& a) {
    return a.bans.vac_banned || a.bans.community_banned ||
           a.bans.game_ban_count > 0 ||
           (a.bans.economy_ban == "banned" || a.bans.economy_ban == "probation");
}

bool passes(const Account& a, const AccountFilter& f, const std::string& q_lower) {
    if (!matches_query(a, q_lower))      return false;
    if (!has_all_tags(a, f.required_tag_ids)) return false;
    if (f.only_with_vac && !a.bans.vac_banned) return false;
    if (f.only_with_any_ban && !any_ban(a)) return false;
    if (f.only_with_sda && !a.sda.has_value()) return false;
    if (f.only_with_cooldown) {
        if (a.cs2.cooldown_expires_unix == 0) return false;
        if (f.now_unix > 0 && a.cs2.cooldown_expires_unix <= f.now_unix) return false;
    }
    if (f.only_with_prime && !a.cs2.prime_status) return false;
    if (f.exclude_red && a.trust == TrustLabel::Red) return false;
    if (f.trust_filter != TrustLabel::Unset && a.trust != f.trust_filter) return false;
    return true;
}

int compare_for_sort(const Account& a, const Account& b, SortKey k) {
    switch (k) {
        case SortKey::PersonaAsc:
            return a.web.persona_name.compare(b.web.persona_name);
        case SortKey::PersonaDesc:
            return b.web.persona_name.compare(a.web.persona_name);
        case SortKey::LoginAsc:
            return a.login.compare(b.login);
        case SortKey::LastLoginDesc:
            if (a.last_login_unix == b.last_login_unix) return 0;
            return a.last_login_unix > b.last_login_unix ? -1 : 1;
        case SortKey::CreatedDesc:
            if (a.created_unix == b.created_unix) return 0;
            return a.created_unix > b.created_unix ? -1 : 1;
        case SortKey::PremierDesc:
            if (a.cs2.premier_rating == b.cs2.premier_rating) return 0;
            return a.cs2.premier_rating > b.cs2.premier_rating ? -1 : 1;
        case SortKey::SteamLevelDesc:
            if (a.web.steam_level == b.web.steam_level) return 0;
            return a.web.steam_level > b.web.steam_level ? -1 : 1;
    }
    return 0;
}

}  // namespace

std::vector<std::size_t> apply_filter(const std::vector<Account>& accounts,
                                      const AccountFilter& filter) {
    const std::string q_lower = to_lower(filter.query);
    std::vector<std::size_t> out;
    out.reserve(accounts.size());
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        if (passes(accounts[i], filter, q_lower)) out.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [&](std::size_t a, std::size_t b) {
        return compare_for_sort(accounts[a], accounts[b], filter.sort) < 0;
    });
    return out;
}

}  // namespace sam::core
