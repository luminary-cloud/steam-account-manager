#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sam::cleaner::vdf {

// A second text-VDF implementation, distinct from sam::steam_local's: that one is narrow and
// exists to patch a handful of known keys, this one is wide and round-trips a whole document so
// the cleaner can delete arbitrary subtrees out of loginusers.vdf / config.vdf / local.vdf
// without losing the fields it doesn't understand.

class Node;

// Children are an ordered vector of (key, node) pairs: Steam VDF allows duplicate keys in some
// files, and the original order matters for round-trip serialization.
using Children = std::vector<std::pair<std::wstring, std::unique_ptr<Node>>>;

class Node {
public:
    Node();
    explicit Node(std::wstring leaf);
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) noexcept = default;
    Node& operator=(Node&&) noexcept = default;

    bool is_object() const noexcept { return std::holds_alternative<Children>(data_); }
    bool is_value() const noexcept { return std::holds_alternative<std::wstring>(data_); }

    Children& children();
    const Children& children() const;

    std::wstring& value();
    const std::wstring& value() const;

    Node* find(std::wstring_view key);
    const Node* find(std::wstring_view key) const;
    Node& set(std::wstring key, std::unique_ptr<Node> child);
    bool remove(std::wstring_view key);

private:
    std::variant<Children, std::wstring> data_;
};

struct Document {
    std::wstring root_key;
    std::unique_ptr<Node> root;

    Document();
};

struct ParseError {
    std::size_t line = 1;
    std::size_t column = 1;
    std::string message;
};

// Tolerates a BOM, // line comments, and CRLF / LF. Conditional suffixes like `[$WIN32]` are
// accepted and ignored. `err`, when given, is filled on failure.
std::optional<Document> parse(std::wstring_view text, ParseError* err = nullptr);

// UTF-8 with an optional BOM, which is what Steam writes.
std::optional<Document> load(const std::filesystem::path& path, ParseError* err = nullptr);

// Valve's tab-indented quoted-string style.
std::wstring serialize(const Document& doc);

bool save(const Document& doc, const std::filesystem::path& path);

}  // namespace sam::cleaner::vdf
