#include "core/steam_api/ban_check.hpp"

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "core/log.hpp"

namespace sam::steam_api {

namespace {

std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string join_ids(const std::vector<std::uint64_t>& ids, std::size_t off, std::size_t n) {
    std::string out;
    out.reserve(n * 18);
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) out += ',';
        out += std::to_string(ids[off + i]);
    }
    return out;
}

}  // namespace

std::unordered_map<std::uint64_t, core::BanStatus> fetch_bans(
    const WebApiConfig& cfg,
    const std::vector<std::uint64_t>& steam_ids) {

    std::unordered_map<std::uint64_t, core::BanStatus> result;
    if (steam_ids.empty() || cfg.api_key.empty()) {
        SAM_LOG_INFO("ban_check: skipped (ids={} api_key_set={})",
                     steam_ids.size(), !cfg.api_key.empty());
        return result;
    }

    SAM_LOG_INFO("ban_check: fetching for {} ids", steam_ids.size());
    const std::int64_t now = now_seconds();

    for (std::size_t off = 0; off < steam_ids.size(); off += 100) {
        const std::size_t batch = std::min<std::size_t>(100, steam_ids.size() - off);
        std::map<std::string, std::string> params{{"steamids", join_ids(steam_ids, off, batch)}};

        const auto resp = get(cfg, "ISteamUser", "GetPlayerBans", "v1", params, true);
        if (resp.status != 200) {
            SAM_LOG_WARN("ban_check: status {} ({})", resp.status, resp.error_message);
            continue;
        }

        try {
            const auto j = nlohmann::json::parse(resp.body);
            if (!j.contains("players")) continue;
            for (const auto& p : j["players"]) {
                core::BanStatus b;
                b.community_banned    = p.value("CommunityBanned", false);
                b.vac_banned          = p.value("VACBanned", false);
                b.vac_ban_count       = p.value("NumberOfVACBans", 0);
                b.game_ban_count      = p.value("NumberOfGameBans", 0);
                b.days_since_last_ban = p.value("DaysSinceLastBan", 0);
                b.economy_ban         = p.value("EconomyBan", std::string{"none"});
                b.last_refreshed_unix = now;

                const std::uint64_t id = parse_u64(p.value("SteamId", nlohmann::json{}));
                if (id != 0) {
                    SAM_LOG_INFO(
                        "ban_check: steam_id={} vac={} vac_bans={} game_bans={} community={} economy='{}' days_since={}",
                        id, b.vac_banned, b.vac_ban_count, b.game_ban_count,
                        b.community_banned, b.economy_ban, b.days_since_last_ban);
                    result[id] = b;
                }
            }
        } catch (const std::exception& ex) {
            SAM_LOG_WARN("ban_check: parse failed: {}", ex.what());
        }
    }

    SAM_LOG_INFO("ban_check: done, {} returned", result.size());
    return result;
}

}  // namespace sam::steam_api
