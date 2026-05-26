#include "core/steam_api/summaries.hpp"

#include <chrono>

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

std::unordered_map<std::uint64_t, core::WebProfile> fetch_summaries(
    const WebApiConfig& cfg,
    const std::vector<std::uint64_t>& steam_ids) {

    std::unordered_map<std::uint64_t, core::WebProfile> result;
    if (steam_ids.empty() || cfg.api_key.empty()) {
        SAM_LOG_INFO("summaries: skipped (ids={} api_key_set={})",
                     steam_ids.size(), !cfg.api_key.empty());
        return result;
    }

    SAM_LOG_INFO("summaries: fetching for {} ids", steam_ids.size());
    const std::int64_t now = now_seconds();

    for (std::size_t off = 0; off < steam_ids.size(); off += 100) {
        const std::size_t batch = std::min<std::size_t>(100, steam_ids.size() - off);
        std::map<std::string, std::string> params{
            {"steamids", join_ids(steam_ids, off, batch)},
        };

        const auto resp = get(cfg, "ISteamUser", "GetPlayerSummaries", "v2", params, true);
        if (resp.status != 200) {
            SAM_LOG_WARN("summaries: status {}", resp.status);
            continue;
        }

        try {
            const auto j = nlohmann::json::parse(resp.body);
            if (!j.contains("response") || !j["response"].contains("players")) continue;
            for (const auto& p : j["response"]["players"]) {
                core::WebProfile w;
                w.persona_name              = p.value("personaname", "");
                w.profile_url               = p.value("profileurl", "");
                w.avatar_url_full           = p.value("avatarfull", "");
                w.country_code              = p.value("loccountrycode", "");
                w.created_unix              = p.value("timecreated", 0);
                w.community_visibility_state = p.value("communityvisibilitystate", 0);
                w.profile_state             = p.value("profilestate", 0);
                w.last_refreshed_unix       = now;

                const std::uint64_t id = parse_u64(p.value("steamid", nlohmann::json{}));
                if (id != 0) {
                    SAM_LOG_INFO(
                        "summaries: steam_id={} persona='{}' country='{}' visibility={} created={} avatar='{}'",
                        id, w.persona_name, w.country_code,
                        w.community_visibility_state, w.created_unix, w.avatar_url_full);
                    result[id] = w;
                }
            }
        } catch (const std::exception& ex) {
            SAM_LOG_WARN("summaries: parse failed: {}", ex.what());
        }
    }

    SAM_LOG_INFO("summaries: done, {} returned", result.size());
    return result;
}

std::optional<int> fetch_steam_level(const WebApiConfig& cfg, std::uint64_t steam_id) {
    if (cfg.api_key.empty() || steam_id == 0) {
        SAM_LOG_INFO("steam_level: skipped (steam_id={} api_key_set={})",
                     steam_id, !cfg.api_key.empty());
        return std::nullopt;
    }

    SAM_LOG_INFO("steam_level: fetching for steam_id={}", steam_id);
    const auto resp = get(cfg, "IPlayerService", "GetSteamLevel", "v1",
                          {{"steamid", std::to_string(steam_id)}}, true);
    if (resp.status != 200) {
        SAM_LOG_WARN("steam_level: status {} for steam_id={}", resp.status, steam_id);
        return std::nullopt;
    }

    try {
        const auto j = nlohmann::json::parse(resp.body);
        if (!j.contains("response")) {
            SAM_LOG_WARN("steam_level: response missing for steam_id={}", steam_id);
            return std::nullopt;
        }
        const auto& r = j["response"];
        if (!r.contains("player_level")) {
            SAM_LOG_INFO("steam_level: steam_id={} no player_level field", steam_id);
            return std::nullopt;
        }
        const int level = r["player_level"].get<int>();
        SAM_LOG_INFO("steam_level: steam_id={} level={}", steam_id, level);
        return level;
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("steam_level: parse failed for steam_id={}: {}", steam_id, ex.what());
        return std::nullopt;
    }
}

std::optional<OwnedGamesInfo> fetch_owned_games(const WebApiConfig& cfg, std::uint64_t steam_id) {
    if (cfg.api_key.empty() || steam_id == 0) {
        SAM_LOG_INFO("owned_games: skipped (steam_id={} api_key_set={})",
                     steam_id, !cfg.api_key.empty());
        return std::nullopt;
    }

    SAM_LOG_INFO("owned_games: fetching for steam_id={}", steam_id);
    const auto resp = get(cfg, "IPlayerService", "GetOwnedGames", "v1",
                          {
                              {"steamid", std::to_string(steam_id)},
                              {"include_appinfo", "0"},
                              {"include_played_free_games", "1"},
                          }, true);
    if (resp.status != 200) {
        SAM_LOG_WARN("owned_games: status {} for steam_id={}", resp.status, steam_id);
        return std::nullopt;
    }

    try {
        const auto j = nlohmann::json::parse(resp.body);
        if (!j.contains("response")) {
            SAM_LOG_WARN("owned_games: response missing for steam_id={}", steam_id);
            return std::nullopt;
        }
        const auto& r = j["response"];
        OwnedGamesInfo info;
        info.games_count = r.value("game_count", 0);
        if (r.contains("games") && r["games"].is_array()) {
            for (const auto& g : r["games"]) {
                info.total_playtime_minutes += g.value("playtime_forever", 0);
            }
        }
        SAM_LOG_INFO("owned_games: steam_id={} games={} playtime_min={}",
                     steam_id, info.games_count, info.total_playtime_minutes);
        return info;
    } catch (const std::exception& ex) {
        SAM_LOG_WARN("owned_games: parse failed for steam_id={}: {}", steam_id, ex.what());
        return std::nullopt;
    }
}

}  // namespace sam::steam_api
