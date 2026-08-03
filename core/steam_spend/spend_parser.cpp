#include "core/steam_spend/spend_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace sam::steam_spend {

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

bool equals_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<Row> rows_from_html(std::string_view html) {
    std::vector<Row> rows;
    static const std::regex row_re(R"(<tr[^>]*>([\s\S]*?)</tr>)", std::regex::icase);
    static const std::regex cell_re(R"(<t[dh][^>]*>([\s\S]*?)</t[dh]>)", std::regex::icase);

    const std::string buf(html);
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

std::int64_t parse_money_cents(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), ','), s.end());
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    if (!s.empty() && s.front() == '$') s.erase(s.begin());
    if (s.empty()) return -1;

    const auto dot = s.find('.');
    std::string whole_s = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac_s  = (dot == std::string::npos) ? std::string{} : s.substr(dot + 1);
    if (whole_s.empty()) whole_s = "0";
    if (frac_s.size() > 2) frac_s.resize(2);
    while (frac_s.size() < 2) frac_s.push_back('0');

    std::int64_t whole = 0;
    auto [p1, e1] = std::from_chars(whole_s.data(), whole_s.data() + whole_s.size(), whole);
    if (e1 != std::errc{} || p1 != whole_s.data() + whole_s.size() || whole < 0) return -1;
    std::int64_t frac = 0;
    auto [p2, e2] = std::from_chars(frac_s.data(), frac_s.data() + frac_s.size(), frac);
    if (e2 != std::errc{} || p2 != frac_s.data() + frac_s.size()) return -1;
    return whole * 100 + frac;
}

}  // namespace

SpendData parse_account_spend(std::string_view html) {
    SpendData out;
    const auto rows = rows_from_html(html);
    if (rows.empty()) return out;
    out.ok = true;

    for (const auto& r : rows) {
        if (r.cells.empty()) continue;

        if (!equals_ci(r.cells[0], "TotalSpend")) continue;
        out.found_total = true;

        if (r.cells.size() > 2) out.total_spend_cents = parse_money_cents(r.cells[2]);
        if (r.cells.size() > 3) {
            out.currency = r.cells[3];
            out.currency_is_usd = equals_ci(r.cells[3], "USD");
        }
        break;
    }
    return out;
}

bool looks_like_spend_page(std::string_view html) {
    return contains_ci(html, "TotalSpend") || contains_ci(html, "PackageOnlySpend");
}

bool looks_like_login_page(std::string_view html) {

    return contains_ci(html, "g_steamID = false") ||
           contains_ci(html, "<title>Sign In");
}

}  // namespace sam::steam_spend
