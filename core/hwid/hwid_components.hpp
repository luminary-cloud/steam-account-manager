#pragma once

#include <cstdint>

namespace sam::hwid {

enum HwidComponent : std::uint32_t {
    kMachineGuid = 1u << 0,
    kMac         = 1u << 1,
    kDiskSerial  = 1u << 2,
    kPcName      = 1u << 3,
    kGpu         = 1u << 4,
    kMotherboard = 1u << 5,
    kRam         = 1u << 6,
    kMonitor     = 1u << 7,
    kStorage     = 1u << 8,
    kSoundCard   = 1u << 9,
    kAll         = 0x3FFu,
};

}  // namespace sam::hwid
