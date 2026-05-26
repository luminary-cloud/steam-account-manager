#pragma once

#include <cstdint>
#include <string>

namespace sam::core {

struct Group {
    std::string id;
    std::string name;
    std::uint32_t color_rgba = 0x4F8EF7FFu;

    bool operator==(const Group&) const = default;
};

}  // namespace sam::core
