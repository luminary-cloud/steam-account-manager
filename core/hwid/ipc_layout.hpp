#pragma once

#include <cstdint>

namespace sam::hwid {

struct HwidIpc {
    char shared_mem_name[64];
    char machine_guid[64];
    char mac_address[24];
    char disk_serial[48];
    char pc_name[24];
    char gpu_name[64];
    char gpu_vendor_id[12];
    char gpu_device_id[12];
    std::int32_t gpu_vram_mb;
    char board_model[64];
    char board_manufacturer[64];
    std::int32_t ram_mb;
    std::int32_t monitor_width;
    std::int32_t monitor_height;
    std::int32_t monitor_refresh;
    std::int32_t storage_ssds;
    char storage_ssd_size[16];
    std::int32_t storage_hdds;
    char storage_hdd_size[16];
    char sound_card[64];
    char display_model[64];
    std::uint32_t spoof_mask;
};

}  // namespace sam::hwid
