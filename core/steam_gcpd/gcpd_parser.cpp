#include "core/steam_gcpd/gcpd_parser.hpp"
#include "core/account_store/account.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace sam::steam_gcpd {

namespace {

struct Row {
    std::vector<std::string> cells;
};

std::string strip_tags(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool in_tag = false;
    for (char c : in) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) {
            if (c == '\n' || c == '\r' || c == '\t') continue;
            out += c;
        }
    }
    static const std::vector<std::pair<std::string, std::string>> entities{
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "},
    };
    for (const auto& [from, to] : entities) {
        std::size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    const auto first = std::find_if(out.begin(), out.end(),
                                    [](unsigned char c) { return !std::isspace(c); });
    const auto last = std::find_if(out.rbegin(), out.rend(),
                                   [](unsigned char c) { return !std::isspace(c); }).base();
    return (first < last) ? std::string(first, last) : std::string{};
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool eq = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                eq = false;
                break;
            }
        }
        if (eq) return true;
    }
    return false;
}

int to_int(std::string s, int dflt = -1) {
    s.erase(std::remove(s.begin(), s.end(), ','), s.end());
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    int v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} ? v : dflt;
}

std::vector<Row> rows_from_table(std::string_view table_html) {
    std::vector<Row> rows;
    static const std::regex row_re(R"(<tr[^>]*>([\s\S]*?)</tr>)", std::regex::icase);
    static const std::regex cell_re(R"(<t[dh][^>]*>([\s\S]*?)</t[dh]>)", std::regex::icase);

    const std::string buf(table_html);
    for (auto it = std::sregex_iterator(buf.begin(), buf.end(), row_re);
         it != std::sregex_iterator(); ++it) {
        Row r;
        const std::string row_html = (*it)[1].str();
        for (auto cit = std::sregex_iterator(row_html.begin(), row_html.end(), cell_re);
             cit != std::sregex_iterator(); ++cit) {
            r.cells.push_back(strip_tags((*cit)[1].str()));
        }
        if (!r.cells.empty()) rows.push_back(std::move(r));
    }
    return rows;
}

std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// Parses Steam's "YYYY-MM-DD HH:MM:SS GMT" format. Returns 0 on failure.
std::int64_t parse_gcpd_timestamp(const std::string& s) {
    if (s.empty()) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d",
                    &y, &mo, &d, &h, &mi, &se) != 6) {
        return 0;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = se;
#ifdef _WIN32
    const std::time_t t = _mkgmtime(&tm);
#else
    const std::time_t t = timegm(&tm);
#endif
    return (t == -1) ? 0 : static_cast<std::int64_t>(t);
}

// All `<table class="generic_kv_table">` elements as ordered lists of rows,
// header row included as the first entry. Mirrors what we see in DevTools.
std::vector<std::vector<Row>> extract_tables(std::string_view html) {
    std::vector<std::vector<Row>> out;
    static const std::regex tbl_re(
        R"(<table[^>]*class\s*=\s*"[^"]*generic_kv_table[^"]*"[^>]*>([\s\S]*?)</table>)",
        std::regex::icase);
    const std::string buf(html);
    for (auto it = std::sregex_iterator(buf.begin(), buf.end(), tbl_re);
         it != std::sregex_iterator(); ++it) {
        out.push_back(rows_from_table((*it)[1].str()));
    }
    return out;
}

// True if the table's header (first row) starts with the given keyword in
// cell 0, case-insensitive.
bool header_starts_with(const std::vector<Row>& tbl, std::string_view kw) {
    if (tbl.empty() || tbl.front().cells.empty()) return false;
    return contains_ci(tbl.front().cells.front(), kw);
}

bool header_contains_any_cell(const std::vector<Row>& tbl,
                              std::initializer_list<const char*> needles) {
    if (tbl.empty()) return false;
    for (const auto& cell : tbl.front().cells) {
        for (const char* n : needles) {
            if (contains_ci(cell, n)) return true;
        }
    }
    return false;
}

}  // namespace

MatchmakingData parse_matchmaking(std::string_view html) {
    MatchmakingData out;
    const auto tables = extract_tables(html);
    if (tables.empty()) return out;
    out.ok = true;
    const std::int64_t now = now_seconds();

    for (const auto& tbl : tables) {
        if (tbl.size() < 2) continue;   // need header + at least one data row
        const auto& header = tbl.front().cells;
        if (header.empty()) continue;

        // Cooldown table: header[0] is "Competitive Cooldown Expiration",
        // header[1] the Level, header[2] Acknowledged.
        if (contains_ci(header[0], "Cooldown")) {
            for (std::size_t i = 1; i < tbl.size(); ++i) {
                const auto& row = tbl[i].cells;
                if (row.empty()) continue;
                const std::int64_t ts = parse_gcpd_timestamp(row[0]);  // localized "Never" -> 0
                if (ts == 0) {
                    // A non-date expiration ("Never") paired with a real penalty
                    // Level (>= 1) denotes a permanent cooldown. Keying off the
                    // integer Level keeps this locale-agnostic.
                    const bool has_level = row.size() > 1 && to_int(row[1], 0) >= 1;
                    if (!row[0].empty() && has_level) {
                        out.cooldown_expires_unix = sam::core::kCooldownNever;
                        out.cooldown_reason = "Competitive cooldown";
                    }
                    continue;
                }
                // A permanent cooldown outranks any temporary row in the table;
                // ts < kCooldownNever is always true, so guard against clobbering it.
                if (out.cooldown_expires_unix == sam::core::kCooldownNever) continue;
                if (ts > now &&
                    (out.cooldown_expires_unix == 0 || ts < out.cooldown_expires_unix)) {
                    out.cooldown_expires_unix = ts;
                    out.cooldown_reason = "Competitive cooldown";
                }
            }
            continue;
        }

        // Main matchmaking-mode table: header[0] is "Matchmaking Mode".
        // Shape: Mode | Wins | Ties | Losses | Skill Group | Last Match | Region.
        // Skip the per-map variant (header includes Map/Mappa/Mapa/Carte/Karte);
        // we no longer extract per-map ranks.
        if (contains_ci(header[0], "Matchmaking Mode")) {
            if (header_contains_any_cell(tbl, {"Map", "Mappa", "Mapa", "Carte", "Karte"})) {
                continue;
            }
            int skill_col = -1;
            int wins_col  = -1;
            for (std::size_t c = 0; c < header.size(); ++c) {
                if (skill_col < 0 && contains_ci(header[c], "Skill")) skill_col = static_cast<int>(c);
                if (wins_col  < 0 && contains_ci(header[c], "Wins"))  wins_col  = static_cast<int>(c);
            }
            if (skill_col < 0) skill_col = 4;  // canonical position fallback
            if (wins_col  < 0) wins_col  = 1;  // canonical: Mode | Wins | Ties | ...

            for (std::size_t i = 1; i < tbl.size(); ++i) {
                const auto& row = tbl[i].cells;
                if (row.empty()) continue;
                const int skill = (row.size() > static_cast<std::size_t>(skill_col))
                    ? to_int(row[skill_col], -1) : -1;
                const int wins  = (row.size() > static_cast<std::size_t>(wins_col))
                    ? to_int(row[wins_col], -1) : -1;
                if (skill < 0 && wins < 0) continue;   // nothing usable on this row

                if (contains_ci(row[0], "Premier")) {
                    if (skill > 0) out.premier_rating = skill;
                    if (wins >= 0) out.premier_wins   = wins;
                } else if (contains_ci(row[0], "Wingman")) {
                    if (skill > 0) out.wingman_rank = skill;
                    if (wins >= 0) out.wingman_wins = wins;
                }
                // (Competitive in the main table is the legacy global skill
                // group; we no longer surface it.)
            }
            continue;
        }
    }

    return out;
}

bool looks_like_gcpd_page(std::string_view html) {
    // `generic_kv_table` is the table class every GCPD tab renders; the
    // breadcrumb literal is the heading at the top of every GCPD page.
    return contains_ci(html, "generic_kv_table") ||
           contains_ci(html, "Personal Game Data");
}

bool looks_like_login_page(std::string_view html) {
    // Steam's login HTML always embeds `g_steamID = false;` in an inline
    // script before the user authenticates; the `<title>Sign In` literal
    // covers the rendered title in case the inline script is restyled.
    return contains_ci(html, "g_steamID = false") ||
           contains_ci(html, "<title>Sign In");
}

AccountMainData parse_accountmain(std::string_view html) {
    AccountMainData out;
    const auto tables = extract_tables(html);
    if (tables.empty()) return out;
    out.ok = true;

    // Steam renders accountmain's profile rank as a free-text block inside
    // a single <td> (not as rows). Concatenate ALL cell text across all
    // tables and grep for the labelled values.
    std::string blob;
    for (const auto& tbl : tables) {
        for (const auto& row : tbl) {
            for (const auto& cell : row.cells) {
                blob += cell;
                blob += '\n';
            }
        }
    }

    auto extract_int_after = [&](std::initializer_list<const char*> labels) -> int {
        for (const char* lbl : labels) {
            const std::string needle = std::string(lbl) + ":";
            auto pos = std::string_view{blob}.find(needle);
            while (pos != std::string_view::npos) {
                pos += needle.size();
                while (pos < blob.size() && std::isspace(static_cast<unsigned char>(blob[pos]))) ++pos;
                int v = 0;
                auto [end, ec] = std::from_chars(blob.data() + pos, blob.data() + blob.size(), v);
                if (ec == std::errc{}) return v;
                pos = std::string_view{blob}.find(needle, pos);
            }
        }
        return -1;
    };

    out.cs2_player_level = extract_int_after({"CS:GO Profile Rank",
                                              "CS2 Profile Rank",
                                              "Profile Rank",
                                              "Player Level"});
    out.cs2_player_xp    = extract_int_after({"Experience points earned towards next rank",
                                              "Player XP",
                                              "XP"});
    return out;
}

}  // namespace sam::steam_gcpd
