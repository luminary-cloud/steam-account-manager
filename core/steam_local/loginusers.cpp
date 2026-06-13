#include "core/steam_local/loginusers.hpp"

#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/strings.hpp"
#include "platform/fs.hpp"
#include "platform/registry.hpp"

namespace sam::steam_local {

namespace {

// Minimal text-VDF tokenizer: quoted strings, { } braces, and // line comments.
struct Token {
    enum Kind { String, LBrace, RBrace, End };
    Kind kind = End;
    std::string value;
};

class Tokenizer {
public:
    explicit Tokenizer(std::string_view src) : src_(src) {}

    Token next() {
        skip_ws_and_comments();
        if (pos_ >= src_.size()) return {Token::End, {}};

        char c = src_[pos_];
        if (c == '{') { ++pos_; return {Token::LBrace, {}}; }
        if (c == '}') { ++pos_; return {Token::RBrace, {}}; }
        if (c == '"') return read_quoted();

        // Unquoted token: read until whitespace or brace.
        const std::size_t start = pos_;
        while (pos_ < src_.size()) {
            char ch = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(ch)) || ch == '{' || ch == '}') break;
            ++pos_;
        }
        return {Token::String, std::string(src_.substr(start, pos_ - start))};
    }

private:
    void skip_ws_and_comments() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) { ++pos_; continue; }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    Token read_quoted() {
        ++pos_;
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return {Token::String, std::move(out)};
            if (c == '\\' && pos_ < src_.size()) {
                char e = src_[pos_++];
                switch (e) {
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    default:   out.push_back(e);    break;
                }
                continue;
            }
            out.push_back(c);
        }
        return {Token::String, std::move(out)};  // unterminated; best-effort
    }

    std::string_view src_;
    std::size_t pos_ = 0;
};

std::string read_file_to_string(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

using core::to_lower;

void skip_block(Tokenizer& tk) {
    int depth = 1;
    while (depth > 0) {
        Token t = tk.next();
        if (t.kind == Token::End) return;
        if (t.kind == Token::LBrace) ++depth;
        else if (t.kind == Token::RBrace) --depth;
    }
}

void parse_users_block(Tokenizer& tk, std::vector<LocalAccount>& out) {
    while (true) {
        Token key = tk.next();
        if (key.kind == Token::RBrace || key.kind == Token::End) return;
        if (key.kind != Token::String) continue;

        Token open = tk.next();
        if (open.kind != Token::LBrace) {
            continue;
        }

        LocalAccount acc;
        try { acc.steam_id_64 = std::stoull(key.value); } catch (...) { acc.steam_id_64 = 0; }

        while (true) {
            Token k = tk.next();
            if (k.kind == Token::RBrace || k.kind == Token::End) break;
            if (k.kind != Token::String) continue;

            Token v = tk.next();
            if (v.kind == Token::LBrace) {
                skip_block(tk);
                continue;
            }
            if (v.kind != Token::String) {
                if (v.kind == Token::RBrace || v.kind == Token::End) break;
                continue;
            }

            if (k.value == "AccountName") {
                acc.account_name = to_lower(v.value);
            } else if (k.value == "PersonaName") {
                acc.persona_name = v.value;
            } else if (k.value == "Timestamp") {
                try { acc.timestamp = std::stoll(v.value); } catch (...) {}
            }
        }

        if (acc.steam_id_64 != 0 && !acc.account_name.empty()) {
            out.push_back(std::move(acc));
        }
    }
}

std::optional<std::filesystem::path> loginusers_path() {
    auto dir = platform::registry::read_steam_install_dir();
    if (!dir) return std::nullopt;
    auto p = *dir / "config" / "loginusers.vdf";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return std::nullopt;
    return p;
}

}  // namespace

std::vector<LocalAccount> read_loginusers() {
    auto path = loginusers_path();
    if (!path) {
        SAM_LOG_DEBUG("loginusers: file not found (Steam install not detected or file missing)");
        return {};
    }

    std::string text = read_file_to_string(*path);
    if (text.empty()) {
        SAM_LOG_WARN("loginusers: could not read {}", path->string());
        return {};
    }

    std::vector<LocalAccount> out;
    Tokenizer tk(text);

    // Tolerate the "users" key appearing anywhere at top level.
    while (true) {
        Token t = tk.next();
        if (t.kind == Token::End) break;
        if (t.kind != Token::String) continue;
        const std::string key = t.value;

        Token v = tk.next();
        if (v.kind != Token::LBrace) continue;

        if (key == "users") {
            parse_users_block(tk, out);
            break;
        }
        skip_block(tk);
    }

    SAM_LOG_DEBUG("loginusers: parsed {} entries from {}", out.size(), path->string());
    return out;
}

std::uint64_t lookup_steam_id(std::string_view login) {
    if (login.empty()) return 0;
    const std::string needle = to_lower(login);
    const auto users = read_loginusers();
    for (const auto& u : users) {
        if (u.account_name == needle) return u.steam_id_64;
    }
    return 0;
}

namespace {

void parse_object_body(Tokenizer& tk, VdfNode& parent) {
    while (true) {
        Token k = tk.next();
        if (k.kind == Token::End || k.kind == Token::RBrace) return;
        if (k.kind != Token::String) continue;

        VdfNode child;
        child.key = std::move(k.value);

        Token v = tk.next();
        if (v.kind == Token::LBrace) {
            child.is_block = true;
            parse_object_body(tk, child);
        } else if (v.kind == Token::String) {
            child.is_block = false;
            child.value = std::move(v.value);
        } else if (v.kind == Token::RBrace || v.kind == Token::End) {
            parent.children.push_back(std::move(child));
            return;
        } else {
            continue;
        }
        parent.children.push_back(std::move(child));
    }
}

}  // namespace

VdfNode parse_vdf(std::string_view text) {
    VdfNode root;
    root.is_block = true;
    Tokenizer tk(text);
    parse_object_body(tk, root);
    return root;
}

// Only \ and " are escaped, matching what Valve writes.
void write_escaped(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
}

void serialize_node(const VdfNode& n, std::string& out, int depth) {
    const std::string indent(depth, '\t');
    out += indent;
    write_escaped(out, n.key);
    if (n.is_block) {
        out += "\n";
        out += indent;
        out += "{\n";
        for (const auto& c : n.children) serialize_node(c, out, depth + 1);
        out += indent;
        out += "}\n";
    } else {
        out += "\t\t";
        write_escaped(out, n.value);
        out += "\n";
    }
}

VdfNode* find_child(VdfNode& parent, std::string_view key) {
    for (auto& c : parent.children) {
        if (c.key == key) return &c;
    }
    return nullptr;
}

void upsert_scalar(VdfNode& parent, std::string_view key, std::string_view value) {
    if (auto* existing = find_child(parent, key)) {
        existing->is_block = false;
        existing->value = std::string(value);
        existing->children.clear();
        return;
    }
    VdfNode node;
    node.key = std::string(key);
    node.value = std::string(value);
    node.is_block = false;
    parent.children.push_back(std::move(node));
}

bool set_remembered_account(std::uint64_t steam_id_64) {
    if (steam_id_64 == 0) return false;
    auto path = loginusers_path();
    if (!path) {
        SAM_LOG_WARN("set_remembered_account: loginusers.vdf not found");
        return false;
    }

    std::string text = read_file_to_string(*path);
    if (text.empty()) {
        SAM_LOG_WARN("set_remembered_account: empty/unreadable {}", path->string());
        return false;
    }

    VdfNode root = parse_vdf(text);
    VdfNode* users = find_child(root, "users");
    if (!users || !users->is_block) {
        SAM_LOG_WARN("set_remembered_account: no users block in {}", path->string());
        return false;
    }

    const std::string target_sid = std::to_string(steam_id_64);
    bool matched = false;
    for (auto& entry : users->children) {
        if (!entry.is_block) continue;
        const bool is_target = entry.key == target_sid;
        // Steam writes "1"/"0" as quoted strings; match that exactly.
        upsert_scalar(entry, "RememberPassword", is_target ? "1" : "0");
        upsert_scalar(entry, "AllowAutoLogin",  is_target ? "1" : "0");
        upsert_scalar(entry, "MostRecent",      is_target ? "1" : "0");
        if (is_target) matched = true;
    }

    if (!matched) {
        SAM_LOG_WARN("set_remembered_account: steam_id {} not present in {}",
                     target_sid, path->string());
        return false;
    }

    std::string serialized;
    // loginusers.vdf has no wrapping root; write each root child at depth 0.
    for (const auto& c : root.children) serialize_node(c, serialized, 0);

    try {
        platform::atomic_write_file(*path,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(serialized.data()),
                serialized.size()));
    } catch (const std::exception& ex) {
        SAM_LOG_ERROR("set_remembered_account: write failed: {}", ex.what());
        return false;
    }

    SAM_LOG_INFO("set_remembered_account: flagged {} as auto-login in {}",
                 target_sid, path->string());
    return true;
}

bool ensure_loginusers_entry(std::uint64_t steam_id_64,
                             const std::string& account_name,
                             const std::string& persona_name) {
    if (steam_id_64 == 0 || account_name.empty()) return false;
    auto dir = platform::registry::read_steam_install_dir();
    if (!dir) {
        SAM_LOG_WARN("ensure_loginusers_entry: Steam install dir not found");
        return false;
    }
    const auto path = *dir / "config" / "loginusers.vdf";

    // parse_vdf("") yields an empty root when the file doesn't exist yet.
    VdfNode root = parse_vdf(read_file_to_string(path));

    VdfNode* users = find_child(root, "users");
    if (!users) {
        VdfNode u;
        u.key = "users";
        u.is_block = true;
        root.children.push_back(std::move(u));
        users = find_child(root, "users");
    }
    users->is_block = true;

    const std::string target_sid = std::to_string(steam_id_64);
    VdfNode* entry = find_child(*users, target_sid);
    if (!entry) {
        VdfNode e;
        e.key = target_sid;
        e.is_block = true;
        users->children.push_back(std::move(e));
        entry = find_child(*users, target_sid);
    }
    entry->is_block = true;

    upsert_scalar(*entry, "AccountName", account_name);
    if (!persona_name.empty()) upsert_scalar(*entry, "PersonaName", persona_name);
    upsert_scalar(*entry, "RememberPassword", "1");
    upsert_scalar(*entry, "AllowAutoLogin", "1");
    upsert_scalar(*entry, "MostRecent", "1");
    upsert_scalar(*entry, "Timestamp",
                  std::to_string(static_cast<long long>(std::time(nullptr))));

    // Steam auto-logs into exactly one account: clear the flags on the rest.
    for (auto& child : users->children) {
        if (!child.is_block || child.key == target_sid) continue;
        upsert_scalar(child, "RememberPassword", "0");
        upsert_scalar(child, "AllowAutoLogin", "0");
        upsert_scalar(child, "MostRecent", "0");
    }

    std::string serialized;
    for (const auto& c : root.children) serialize_node(c, serialized, 0);

    try {
        platform::atomic_write_file(path,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(serialized.data()),
                serialized.size()));
    } catch (const std::exception& ex) {
        SAM_LOG_ERROR("ensure_loginusers_entry: write failed: {}", ex.what());
        return false;
    }
    SAM_LOG_INFO("ensure_loginusers_entry: wrote entry for {} ({}) in {}",
                 target_sid, account_name, path.string());
    return true;
}

}  // namespace sam::steam_local
