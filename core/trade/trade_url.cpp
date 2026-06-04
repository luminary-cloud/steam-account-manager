#include "core/trade/trade_url.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace sam::core::trade {

namespace {

constexpr std::uint64_t kIndividualBase = 0x0110000100000000ULL;

// Returns the value of query key `key` in `url`, or empty if absent. Accepts a
// full URL or a bare query string.
std::string query_value(std::string_view url, std::string_view key) {
    const auto qpos = url.find('?');
    std::string_view q =
        qpos == std::string_view::npos ? url : url.substr(qpos + 1);
    std::size_t pos = 0;
    while (pos <= q.size()) {
        const std::size_t amp = q.find('&', pos);
        const std::string_view pair = q.substr(
            pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
            return std::string(pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return {};
}

}  // namespace

TradeUrl parse_trade_url(const std::string& url) {
    TradeUrl out;
    const std::string partner = query_value(url, "partner");
    const std::string token = query_value(url, "token");
    if (partner.empty() || token.empty()) return out;

    std::uint32_t account_id = 0;
    try {
        const unsigned long long v = std::stoull(partner);
        if (v == 0 || v > 0xFFFFFFFFULL) return out;
        account_id = static_cast<std::uint32_t>(v);
    } catch (...) {
        return out;
    }

    out.partner_account_id = account_id;
    out.token = token;
    out.ok = true;
    return out;
}

std::uint64_t account_id_to_steam_id_64(std::uint32_t account_id) {
    return kIndividualBase | static_cast<std::uint64_t>(account_id);
}

std::uint32_t steam_id_64_to_account_id(std::uint64_t steam_id_64) {
    return static_cast<std::uint32_t>(steam_id_64 & 0xFFFFFFFFULL);
}

}  // namespace sam::core::trade
