#pragma once

#include <cstdint>
#include <string>

namespace sam::core {

struct Tag {
    std::string id;
    std::string name;
    std::uint32_t color_rgba = 0x4F8EF7FFu;  // muted blue default

    bool operator==(const Tag&) const = default;
};

}  // namespace sam::core
