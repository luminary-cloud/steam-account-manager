#include "core/hwid/hwid_profile.hpp"

#include <nlohmann/json.hpp>

namespace sam::core::hwid {

using json = nlohmann::json;

void to_json(json& j, const HwidProfile& p) {
    j = json::object();
    j["machine_guid"]       = p.machine_guid;
    j["mac_address"]        = p.mac_address;
    j["disk_serial"]        = p.disk_serial;
    j["pc_name"]            = p.pc_name;
    j["gpu_name"]           = p.gpu_name;
    j["gpu_vendor_id"]      = p.gpu_vendor_id;
    j["gpu_device_id"]      = p.gpu_device_id;
    j["gpu_vram_mb"]        = p.gpu_vram_mb;
    j["board_model"]        = p.board_model;
    j["board_manufacturer"] = p.board_manufacturer;
    j["ram_mb"]             = p.ram_mb;
    j["monitor_width"]      = p.monitor_width;
    j["monitor_height"]     = p.monitor_height;
    j["monitor_refresh"]    = p.monitor_refresh;
    j["storage_ssds"]       = p.storage_ssds;
    j["storage_ssd_size"]   = p.storage_ssd_size;
    j["storage_hdds"]       = p.storage_hdds;
    j["storage_hdd_size"]   = p.storage_hdd_size;
    j["sound_card"]         = p.sound_card;
    j["display_model"]      = p.display_model;
}

void from_json(const json& j, HwidProfile& p) {
    p.machine_guid       = j.value("machine_guid", std::string{});
    p.mac_address        = j.value("mac_address", std::string{});
    p.disk_serial        = j.value("disk_serial", std::string{});
    p.pc_name            = j.value("pc_name", std::string{});
    p.gpu_name           = j.value("gpu_name", std::string{});
    p.gpu_vendor_id      = j.value("gpu_vendor_id", std::string{});
    p.gpu_device_id      = j.value("gpu_device_id", std::string{});
    p.gpu_vram_mb        = j.value("gpu_vram_mb", 0);
    p.board_model        = j.value("board_model", std::string{});
    p.board_manufacturer = j.value("board_manufacturer", std::string{});
    p.ram_mb             = j.value("ram_mb", 0);
    p.monitor_width      = j.value("monitor_width", 0);
    p.monitor_height     = j.value("monitor_height", 0);
    p.monitor_refresh    = j.value("monitor_refresh", 0);
    p.storage_ssds       = j.value("storage_ssds", 0);
    p.storage_ssd_size   = j.value("storage_ssd_size", std::string{});
    p.storage_hdds       = j.value("storage_hdds", 0);
    p.storage_hdd_size   = j.value("storage_hdd_size", std::string{});
    p.sound_card         = j.value("sound_card", std::string{});
    p.display_model      = j.value("display_model", std::string{});
}

}  // namespace sam::core::hwid
