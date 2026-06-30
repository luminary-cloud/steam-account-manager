#pragma once

#include <string>
#include <cstdint>

namespace ipc {

struct SessionProfile {
    std::string machine_guid;
    std::string mac_address;
    std::string disk_serial;
    std::string pc_name;
    std::string gpu_name;
    std::string gpu_vendor_id;
    std::string gpu_device_id;
    int32_t     gpu_vram_mb     = 0;
    std::string board_model;
    std::string board_manufacturer;
    int32_t     ram_mb          = 0;
    int32_t     monitor_width   = 0;
    int32_t     monitor_height  = 0;
    int32_t     monitor_refresh = 0;
    int32_t     storage_ssds    = 0;
    std::string storage_ssd_size;
    int32_t     storage_hdds    = 0;
    std::string storage_hdd_size;
    std::string sound_card;
    std::string display_model;
    uint32_t    spoof_mask      = 0;
};

bool read_profile(const char* shared_mem_name);
const SessionProfile& get_profile();

}  // namespace ipc
