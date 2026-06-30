#pragma once

#include <cstdint>
#include <string>

#include "core/hwid/hwid_profile.hpp"

namespace sam::core::hwid {

class HwidSharedMemory {
public:
    HwidSharedMemory() = default;
    ~HwidSharedMemory();

    HwidSharedMemory(const HwidSharedMemory&) = delete;
    HwidSharedMemory& operator=(const HwidSharedMemory&) = delete;
    HwidSharedMemory(HwidSharedMemory&&) noexcept;
    HwidSharedMemory& operator=(HwidSharedMemory&&) noexcept;

    bool publish(const HwidProfile& profile, std::uint32_t spoof_mask = 0x3FFu);

    const std::string& shared_name() const { return name_; }

private:
    void release();

    void* mapping_ = nullptr;
    void* view_ = nullptr;
    std::string name_;
};

}  // namespace sam::core::hwid
