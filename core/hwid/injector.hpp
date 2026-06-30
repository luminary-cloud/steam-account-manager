#pragma once

#include <cstdint>
#include <string>

namespace sam::core::hwid {

struct InjectResult {
    bool ok = false;
    std::string error;
};

InjectResult inject_hwid_dll(std::uint32_t pid, const std::string& shared_mem_name);

}  // namespace sam::core::hwid
