#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace sam::core::hwid {

struct HwidProfile {
    std::string machine_guid;
    std::string mac_address;
    std::string disk_serial;
    std::string pc_name;
    std::string gpu_name;
    std::string gpu_vendor_id;
    std::string gpu_device_id;
    int gpu_vram_mb = 0;
    std::string board_model;
    std::string board_manufacturer;
    int ram_mb = 0;
    int monitor_width = 0;
    int monitor_height = 0;
    int monitor_refresh = 0;
    int storage_ssds = 0;
    std::string storage_ssd_size;
    int storage_hdds = 0;
    std::string storage_hdd_size;
    std::string sound_card;
    std::string display_model;
};

void to_json(nlohmann::json& j, const HwidProfile& p);
void from_json(const nlohmann::json& j, HwidProfile& p);

}  // namespace sam::core::hwid
