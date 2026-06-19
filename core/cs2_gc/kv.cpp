#include "core/cs2_gc/kv.hpp"

#include <cctype>

namespace sam::cs2_gc {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

struct Parser {
    const char* p;
    const char* end;

    void skip_ws() {
        for (;;) {
            while (p < end && static_cast<unsigned char>(*p) <= ' ') ++p;
            if (p + 1 < end && p[0] == '/' && p[1] == '/') {
                p += 2;
                while (p < end && *p != '\n') ++p;
                continue;
            }
            if (p + 1 < end && p[0] == '/' && p[1] == '*') {
                p += 2;
                while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
                if (p + 1 < end) p += 2;
                continue;
            }
            break;
        }
    }

    // Reads one token. Sets `brace` to '{' or '}' when the token is a brace.
    bool next(std::string& out, char& brace) {
        skip_ws();
        brace = 0;
        if (p >= end) return false;
        if (*p == '{' || *p == '}') {
            brace = *p++;
            return true;
        }
        out.clear();
        if (*p == '"') {
            ++p;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    ++p;
                    switch (*p) {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back('\t'); break;
                        case '\\': out.push_back('\\'); break;
                        case '"': out.push_back('"'); break;
                        default: out.push_back(*p); break;
                    }
                    ++p;
                } else {
                    out.push_back(*p++);
                }
            }
            if (p < end) ++p;
        } else {
            while (p < end && static_cast<unsigned char>(*p) > ' ' && *p != '{' && *p != '}' && *p != '"')
                out.push_back(*p++);
        }
        return true;
    }

    void parse_block(std::vector<KvNode>& out) {
        for (;;) {
            std::string key;
            char brace = 0;
            if (!next(key, brace)) return;
            if (brace == '}') return;
            if (brace == '{') continue;  // stray brace without a key

            std::string value;
            char brace2 = 0;
            if (!next(value, brace2)) {
                out.push_back({key, {}, {}});
                return;
            }
            if (brace2 == '{') {
                KvNode node;
                node.key = std::move(key);
                parse_block(node.children);
                out.push_back(std::move(node));
            } else if (brace2 == '}') {
                out.push_back({key, {}, {}});
                return;
            } else {
                out.push_back({std::move(key), std::move(value), {}});
            }
        }
    }
};

}  // namespace

const KvNode* KvNode::find(std::string_view k) const {
    for (const auto& c : children)
        if (iequals(c.key, k)) return &c;
    return nullptr;
}

bool parse_kv(const std::string& text, KvNode& root) {
    root = KvNode{};
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    // Valve's localization files (csgo_english.txt) ship with a UTF-8 BOM. The BOM
    // bytes are > ' ', so skip_ws() won't drop them; left in, the first key reads as
    // a bareword and "lang" becomes its value, so find("lang") fails and no strings
    // load. items_game.txt has no BOM, which is why names half-resolved.
    if (end - begin >= 3 && static_cast<unsigned char>(begin[0]) == 0xEF &&
        static_cast<unsigned char>(begin[1]) == 0xBB && static_cast<unsigned char>(begin[2]) == 0xBF)
        begin += 3;
    Parser ps{begin, end};
    ps.parse_block(root.children);
    return !root.children.empty();
}

}  // namespace sam::cs2_gc
