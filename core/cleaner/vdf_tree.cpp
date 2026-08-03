#include "core/cleaner/vdf_tree.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

namespace sam::cleaner::vdf {

Node::Node() : data_(Children{}) {}

Node::Node(std::wstring leaf) : data_(std::move(leaf)) {}

Children& Node::children() { return std::get<Children>(data_); }

const Children& Node::children() const { return std::get<Children>(data_); }

std::wstring& Node::value() { return std::get<std::wstring>(data_); }

const std::wstring& Node::value() const { return std::get<std::wstring>(data_); }

Node* Node::find(std::wstring_view key) {
    if (!is_object()) return nullptr;
    for (auto& [k, v] : children()) {
        if (k == key) return v.get();
    }
    return nullptr;
}

const Node* Node::find(std::wstring_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& [k, v] : children()) {
        if (k == key) return v.get();
    }
    return nullptr;
}

Node& Node::set(std::wstring key, std::unique_ptr<Node> child) {
    auto& chs = children();
    for (auto& [k, v] : chs) {
        if (k == key) {
            v = std::move(child);
            return *v;
        }
    }
    chs.emplace_back(std::move(key), std::move(child));
    return *chs.back().second;
}

bool Node::remove(std::wstring_view key) {
    if (!is_object()) return false;
    auto& chs = children();
    auto it = std::find_if(chs.begin(), chs.end(), [&](const auto& kv) { return kv.first == key; });
    if (it == chs.end()) return false;
    chs.erase(it);
    return true;
}

Document::Document() : root(std::make_unique<Node>()) {}

namespace {

class Parser {
public:
    explicit Parser(std::wstring_view text) : src_(text) {}

    std::optional<Document> parse(ParseError* out_err) {
        skip_ws();
        Document doc;
        auto key = read_string();
        if (!key) return fail(out_err, "expected root key");
        doc.root_key = std::move(*key);

        skip_ws();
        if (peek() != L'{') return fail(out_err, "expected '{' after root key");
        consume();

        auto root = std::make_unique<Node>();
        if (auto err = parse_object(*root)) {
            if (out_err != nullptr) *out_err = *err;
            return std::nullopt;
        }
        doc.root = std::move(root);
        return doc;
    }

private:
    std::optional<Document> fail(ParseError* out_err, std::string msg) {
        if (out_err != nullptr) *out_err = error(std::move(msg));
        return std::nullopt;
    }

    std::wstring_view src_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t col_ = 1;

    ParseError error(std::string msg) const { return ParseError{line_, col_, std::move(msg)}; }

    wchar_t peek() const { return pos_ < src_.size() ? src_[pos_] : L'\0'; }

    wchar_t peek_next() const { return pos_ + 1 < src_.size() ? src_[pos_ + 1] : L'\0'; }

    wchar_t consume() {
        const wchar_t ch = src_[pos_++];
        if (ch == L'\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        return ch;
    }

    void skip_ws() {
        while (pos_ < src_.size()) {
            const wchar_t ch = peek();
            if (iswspace(ch)) {
                consume();
                continue;
            }
            if (ch == L'/' && peek_next() == L'/') {
                while (pos_ < src_.size() && peek() != L'\n') consume();
                continue;
            }

            if (ch == L'[') {
                while (pos_ < src_.size() && peek() != L']') consume();
                if (pos_ < src_.size()) consume();
                continue;
            }
            break;
        }
    }

    std::optional<std::wstring> read_string() {
        skip_ws();
        if (pos_ >= src_.size()) return std::nullopt;
        if (peek() == L'"') return read_quoted();
        return read_bare();
    }

    std::optional<std::wstring> read_quoted() {
        consume();
        std::wstring out;
        while (pos_ < src_.size()) {
            const wchar_t ch = consume();
            if (ch == L'"') return out;
            if (ch == L'\\' && pos_ < src_.size()) {
                const wchar_t esc = consume();
                switch (esc) {
                    case L'n': out.push_back(L'\n'); break;
                    case L'r': out.push_back(L'\r'); break;
                    case L't': out.push_back(L'\t'); break;
                    case L'\\': out.push_back(L'\\'); break;
                    case L'"': out.push_back(L'"'); break;
                    default: out.push_back(esc); break;
                }
                continue;
            }
            out.push_back(ch);
        }
        return std::nullopt;
    }

    std::optional<std::wstring> read_bare() {
        std::wstring out;
        while (pos_ < src_.size()) {
            const wchar_t ch = peek();
            if (iswspace(ch) || ch == L'{' || ch == L'}' || ch == L'"') break;
            out.push_back(consume());
        }
        return out.empty() ? std::nullopt : std::optional{out};
    }

    std::optional<ParseError> parse_object(Node& node) {
        while (true) {
            skip_ws();
            if (pos_ >= src_.size()) return error("unexpected end of input inside object");
            if (peek() == L'}') {
                consume();
                return std::nullopt;
            }
            auto key = read_string();
            if (!key) return error("expected key string");
            skip_ws();
            if (peek() == L'{') {
                consume();
                auto child = std::make_unique<Node>();
                if (auto err = parse_object(*child)) return *err;
                node.children().emplace_back(std::move(*key), std::move(child));
            } else {
                auto val = read_string();
                if (!val) return error("expected value or '{' after key");
                auto leaf = std::make_unique<Node>(std::move(*val));
                node.children().emplace_back(std::move(*key), std::move(leaf));
            }
        }
    }
};

void escape(const std::wstring& s, std::wstring& out) {
    out.reserve(out.size() + s.size() + 2);
    out.push_back(L'"');
    for (const wchar_t ch : s) {
        switch (ch) {
            case L'\\': out.append(L"\\\\"); break;
            case L'"': out.append(L"\\\""); break;
            case L'\n': out.append(L"\\n"); break;
            case L'\r': out.append(L"\\r"); break;
            case L'\t': out.append(L"\\t"); break;
            default: out.push_back(ch); break;
        }
    }
    out.push_back(L'"');
}

void write_node(const Node& node, std::wstring& out, int indent) {
    const std::wstring pad(static_cast<std::size_t>(indent), L'\t');
    if (!node.is_object()) return;
    for (const auto& [k, v] : node.children()) {
        out.append(pad);
        escape(k, out);
        if (v->is_object()) {
            out.append(L"\n").append(pad).append(L"{\n");
            write_node(*v, out, indent + 1);
            out.append(pad).append(L"}\n");
        } else {
            out.append(L"\t\t");
            escape(v->value(), out);
            out.push_back(L'\n');
        }
    }
}

std::wstring read_file_utf8(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string bytes = ss.str();

    std::size_t start = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        start = 3;
    }

    const int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + start,
                                             static_cast<int>(bytes.size() - start), nullptr, 0);
    if (wide_len <= 0) return {};
    std::wstring out(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + start,
                        static_cast<int>(bytes.size() - start), out.data(), wide_len);
    return out;
}

bool write_file_utf8(const std::filesystem::path& path, std::wstring_view text) {
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return false;
    std::string bytes(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(),
                        needed, nullptr, nullptr);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

}  // namespace

std::optional<Document> parse(std::wstring_view text, ParseError* err) {
    if (!text.empty() && text.front() == 0xFEFF) text.remove_prefix(1);
    Parser p{text};
    return p.parse(err);
}

std::optional<Document> load(const std::filesystem::path& path, ParseError* err) {
    const auto text = read_file_utf8(path);
    if (text.empty()) {
        if (err != nullptr) *err = ParseError{1, 1, "could not read file"};
        return std::nullopt;
    }
    return parse(text, err);
}

std::wstring serialize(const Document& doc) {
    std::wstring out;
    escape(doc.root_key, out);
    out.append(L"\n{\n");
    if (doc.root) write_node(*doc.root, out, 1);
    out.append(L"}\n");
    return out;
}

bool save(const Document& doc, const std::filesystem::path& path) {
    return write_file_utf8(path, serialize(doc));
}

}  // namespace sam::cleaner::vdf
