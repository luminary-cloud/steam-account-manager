#include "ipc_reader.hpp"
#include <windows.h>
#include "core/hwid/ipc_layout.hpp"

static ipc::SessionProfile s_profile;

bool ipc::read_profile(const char* shared_mem_name) {
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shared_mem_name);
    if (!mapping)
        return false;

    auto* data = static_cast<sam::hwid::HwidIpc*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(sam::hwid::HwidIpc)));
    if (!data) {
        CloseHandle(mapping);
        return false;
    }

    s_profile.machine_guid       = data->machine_guid;
    s_profile.mac_address        = data->mac_address;
    s_profile.disk_serial        = data->disk_serial;
    s_profile.pc_name            = data->pc_name;
    s_profile.gpu_name           = data->gpu_name;
    s_profile.gpu_vendor_id      = data->gpu_vendor_id;
    s_profile.gpu_device_id      = data->gpu_device_id;
    s_profile.gpu_vram_mb        = data->gpu_vram_mb;
    s_profile.board_model        = data->board_model;
    s_profile.board_manufacturer = data->board_manufacturer;
    s_profile.ram_mb             = data->ram_mb;
    s_profile.monitor_width      = data->monitor_width;
    s_profile.monitor_height     = data->monitor_height;
    s_profile.monitor_refresh    = data->monitor_refresh;
    s_profile.storage_ssds       = data->storage_ssds;
    s_profile.storage_ssd_size   = data->storage_ssd_size;
    s_profile.storage_hdds       = data->storage_hdds;
    s_profile.storage_hdd_size   = data->storage_hdd_size;
    s_profile.sound_card         = data->sound_card;
    s_profile.display_model      = data->display_model;
    s_profile.spoof_mask         = data->spoof_mask;

    SecureZeroMemory(data, sizeof(sam::hwid::HwidIpc));
    UnmapViewOfFile(data);
    CloseHandle(mapping);
    return true;
}

const ipc::SessionProfile& ipc::get_profile() {
    return s_profile;
}
