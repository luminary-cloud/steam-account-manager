#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sam::cs2_gc {

// A node in a parsed Valve KeyValues document. A node is either a leaf (value set,
// children empty) or a block (children set).
struct KvNode {
    std::string key;
    std::string value;
    std::vector<KvNode> children;

    const KvNode* find(std::string_view k) const;  // first child with key, case-insensitive
};

// Parses KeyValues text. `root.children` holds the top-level keys. Returns false if
// nothing parsed.
bool parse_kv(const std::string& text, KvNode& root);

}  // namespace sam::cs2_gc
