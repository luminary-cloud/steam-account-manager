#include "core/hwid/hwid_ipc.hpp"

#include <array>
#include <cstring>
#include <utility>

#include <windows.h>

#include "core/crypto/rng.hpp"
#include "core/hwid/ipc_layout.hpp"
#include "core/strings.hpp"

namespace sam::core::hwid {

namespace {

void safe_copy(char* dst, std::size_t dst_size, const std::string& src) {
    std::size_t n = (std::min)(src.size(), dst_size - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

std::string make_shared_name() {
    auto bytes = crypto::random_bytes(8);
    return "Local\\Sam_" + to_hex_lower(bytes);
}

}  // namespace

HwidSharedMemory::~HwidSharedMemory() {
    release();
}

HwidSharedMemory::HwidSharedMemory(HwidSharedMemory&& o) noexcept
    : mapping_(std::exchange(o.mapping_, nullptr))
    , view_(std::exchange(o.view_, nullptr))
    , name_(std::move(o.name_)) {}

HwidSharedMemory& HwidSharedMemory::operator=(HwidSharedMemory&& o) noexcept {
    if (this != &o) {
        release();
        mapping_ = std::exchange(o.mapping_, nullptr);
        view_ = std::exchange(o.view_, nullptr);
        name_ = std::move(o.name_);
    }
    return *this;
}

void HwidSharedMemory::release() {
    if (view_) {
        std::memset(view_, 0, sizeof(sam::hwid::HwidIpc));
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    name_.clear();
}

bool HwidSharedMemory::publish(const HwidProfile& profile, std::uint32_t spoof_mask) {
    release();

    name_ = make_shared_name();

    mapping_ = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(sam::hwid::HwidIpc), name_.c_str());
    if (!mapping_) return false;

    view_ = MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(sam::hwid::HwidIpc));
    if (!view_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    auto* ipc = static_cast<sam::hwid::HwidIpc*>(view_);
    std::memset(ipc, 0, sizeof(*ipc));

    safe_copy(ipc->shared_mem_name, sizeof(ipc->shared_mem_name), name_);
    safe_copy(ipc->machine_guid, sizeof(ipc->machine_guid), profile.machine_guid);
    safe_copy(ipc->mac_address, sizeof(ipc->mac_address), profile.mac_address);
    safe_copy(ipc->disk_serial, sizeof(ipc->disk_serial), profile.disk_serial);
    safe_copy(ipc->pc_name, sizeof(ipc->pc_name), profile.pc_name);
    safe_copy(ipc->gpu_name, sizeof(ipc->gpu_name), profile.gpu_name);
    safe_copy(ipc->gpu_vendor_id, sizeof(ipc->gpu_vendor_id), profile.gpu_vendor_id);
    safe_copy(ipc->gpu_device_id, sizeof(ipc->gpu_device_id), profile.gpu_device_id);
    ipc->gpu_vram_mb = profile.gpu_vram_mb;
    safe_copy(ipc->board_model, sizeof(ipc->board_model), profile.board_model);
    safe_copy(ipc->board_manufacturer, sizeof(ipc->board_manufacturer), profile.board_manufacturer);
    ipc->ram_mb = profile.ram_mb;
    ipc->monitor_width = profile.monitor_width;
    ipc->monitor_height = profile.monitor_height;
    ipc->monitor_refresh = profile.monitor_refresh;
    ipc->storage_ssds = profile.storage_ssds;
    safe_copy(ipc->storage_ssd_size, sizeof(ipc->storage_ssd_size), profile.storage_ssd_size);
    ipc->storage_hdds = profile.storage_hdds;
    safe_copy(ipc->storage_hdd_size, sizeof(ipc->storage_hdd_size), profile.storage_hdd_size);
    safe_copy(ipc->sound_card, sizeof(ipc->sound_card), profile.sound_card);
    safe_copy(ipc->display_model, sizeof(ipc->display_model), profile.display_model);
    ipc->spoof_mask = spoof_mask;

    return true;
}

}  // namespace sam::core::hwid
