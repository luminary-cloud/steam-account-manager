#include "core/hwid/hwid_gen.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/crypto/rng.hpp"
#include "core/hwid/hardware_pool.hpp"
#include "core/strings.hpp"

namespace sam::core::hwid {

namespace {

std::size_t pick_index(std::size_t pool_size) {
    std::array<std::uint8_t, 4> buf{};
    crypto::fill_random(buf);
    std::uint32_t val = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    return static_cast<std::size_t>(val % pool_size);
}

std::string make_uuid_v4() {
    std::array<std::uint8_t, 16> bytes{};
    crypto::fill_random(bytes);
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);

    const std::string hex = to_hex_lower({bytes.data(), bytes.size()});
    std::string uuid;
    uuid.reserve(36);
    uuid += hex.substr(0, 8);
    uuid += '-';
    uuid += hex.substr(8, 4);
    uuid += '-';
    uuid += hex.substr(12, 4);
    uuid += '-';
    uuid += hex.substr(16, 4);
    uuid += '-';
    uuid += hex.substr(20, 12);
    return uuid;
}

std::string make_mac() {
    std::array<std::uint8_t, 6> bytes{};
    crypto::fill_random(bytes);
    bytes[0] = (bytes[0] & 0xFC) | 0x02;

    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X-%02X-%02X-%02X-%02X-%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return buf;
}

std::string make_disk_serial() {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::array<std::uint8_t, 8> bytes{};
    crypto::fill_random(bytes);

    std::string serial = "0000_0000_0000_0001_";
    for (int i = 0; i < 8; ++i) {
        if (i > 0 && i % 2 == 0) serial += '_';
        serial += kHex[(bytes[i] >> 4) & 0x0F];
        serial += kHex[bytes[i] & 0x0F];
    }
    serial += '.';
    return serial;
}

std::string make_pc_name() {
    static constexpr char kAlnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::array<std::uint8_t, 7> bytes{};
    crypto::fill_random(bytes);

    std::string name = "DESKTOP-";
    for (int i = 0; i < 7; ++i)
        name += kAlnum[bytes[i] % 36];
    return name;
}

}  // namespace

HwidProfile generate_profile() {
    HwidProfile p;
    p.machine_guid = make_uuid_v4();
    p.mac_address = make_mac();
    p.disk_serial = make_disk_serial();
    p.pc_name = make_pc_name();

    const auto& gpu = kGpus[pick_index(kGpus.size())];
    p.gpu_name = std::string(gpu.name);
    p.gpu_vendor_id = std::string(gpu.vendor_id);
    p.gpu_device_id = std::string(gpu.device_id);
    p.gpu_vram_mb = gpu.vram_mb;

    const auto& board = kBoards[pick_index(kBoards.size())];
    p.board_model = std::string(board.model);
    p.board_manufacturer = std::string(board.manufacturer);

    p.ram_mb = kRam[pick_index(kRam.size())];

    const auto& mon = kMonitors[pick_index(kMonitors.size())];
    p.monitor_width = mon.width;
    p.monitor_height = mon.height;
    p.monitor_refresh = mon.refresh;

    const auto& stor = kStorage[pick_index(kStorage.size())];
    p.storage_ssds = stor.ssds;
    p.storage_ssd_size = std::string(stor.ssd_size);
    p.storage_hdds = stor.hdds;
    p.storage_hdd_size = std::string(stor.hdd_size);

    p.sound_card = std::string(kSoundCards[pick_index(kSoundCards.size())].name);

    p.display_model = std::string(kDisplayModels[pick_index(kDisplayModels.size())].name);

    return p;
}

}  // namespace sam::core::hwid
