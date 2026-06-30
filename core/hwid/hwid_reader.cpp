#include "core/hwid/hwid_reader.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <comdef.h>
#include <dxgi1_2.h>
#include <iphlpapi.h>
#include <mmdeviceapi.h>
#include <wbemidl.h>
#include <wrl/client.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

using Microsoft::WRL::ComPtr;

namespace sam::core::hwid {

namespace {

const PROPERTYKEY kPkeyDeviceFriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string wide_to_narrow(const wchar_t* s, int len = -1) {
    if (!s) return {};
    if (len < 0) len = static_cast<int>(wcslen(s));
    if (len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    return out;
}

std::string read_machine_guid() {
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return {};

    char buf[256]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExA(key, "MachineGuid", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ) return {};
    return std::string(buf);
}

std::string read_mac_address() {
    ULONG buf_len = 0;
    GetAdaptersInfo(nullptr, &buf_len);
    if (buf_len == 0) return {};

    std::vector<std::uint8_t> buf(buf_len);
    auto* info = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
    if (GetAdaptersInfo(info, &buf_len) != NO_ERROR) return {};

    for (auto* a = info; a; a = a->Next) {
        if (a->Type == MIB_IF_TYPE_LOOPBACK) continue;
        if (a->AddressLength < 6) continue;
        char mac[32]{};
        std::snprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X",
                      a->Address[0], a->Address[1], a->Address[2],
                      a->Address[3], a->Address[4], a->Address[5]);
        return std::string(mac);
    }
    return {};
}

std::string read_pc_name() {
    char buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = sizeof(buf);
    if (!GetComputerNameA(buf, &size)) return {};
    return std::string(buf, size);
}

void read_gpu(HwidProfile& p) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, &adapter))) return;

    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) return;

    p.gpu_name = wide_to_narrow(desc.Description);
    char hex[16]{};
    std::snprintf(hex, sizeof(hex), "%04X", desc.VendorId);
    p.gpu_vendor_id = hex;
    std::snprintf(hex, sizeof(hex), "%04X", desc.DeviceId);
    p.gpu_device_id = hex;
    p.gpu_vram_mb = static_cast<int>(desc.DedicatedVideoMemory / 1024 / 1024);
}

int read_total_ram_mb() {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem)) return 0;
    return static_cast<int>(mem.ullTotalPhys / 1024 / 1024);
}

void read_monitor(HwidProfile& p) {
    p.monitor_width = GetSystemMetrics(SM_CXSCREEN);
    p.monitor_height = GetSystemMetrics(SM_CYSCREEN);

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        p.monitor_refresh = static_cast<int>(dm.dmDisplayFrequency);
}

std::string read_display_model() {
    DISPLAY_DEVICEW dev{};
    dev.cb = sizeof(dev);
    if (!EnumDisplayDevicesW(nullptr, 0, &dev, 0)) return {};

    DISPLAY_DEVICEW mon{};
    mon.cb = sizeof(mon);
    if (!EnumDisplayDevicesW(dev.DeviceName, 0, &mon, 0)) return {};

    std::string name = wide_to_narrow(mon.DeviceString);
    if (name.empty() || name == "Generic PnP Monitor")
        name = wide_to_narrow(dev.DeviceString);
    return name;
}

struct SmbiosHeader {
    std::uint8_t type;
    std::uint8_t length;
    std::uint16_t handle;
};

const char* smbios_string(const std::uint8_t* hdr, std::uint8_t hdr_len,
                           std::uint8_t index) {
    if (index == 0) return "";
    const char* p = reinterpret_cast<const char*>(hdr + hdr_len);
    for (std::uint8_t i = 1; i < index; ++i) {
        while (*p) ++p;
        ++p;
    }
    return p;
}

void read_board(HwidProfile& p) {
    DWORD size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (size == 0) return;

    std::vector<std::uint8_t> buf(size);
    if (GetSystemFirmwareTable('RSMB', 0, buf.data(), size) == 0) return;

    const std::uint8_t* data = buf.data() + 8;
    const std::uint8_t* end = buf.data() + size;

    while (data + 4 < end) {
        auto* hdr = reinterpret_cast<const SmbiosHeader*>(data);
        if (hdr->type == 127) break;
        if (hdr->length < 4) break;

        if (hdr->type == 2 && hdr->length >= 8) {
            p.board_manufacturer = smbios_string(data, hdr->length, data[4]);
            p.board_model = smbios_string(data, hdr->length, data[5]);
            return;
        }

        const std::uint8_t* next = data + hdr->length;
        while (next + 1 < end && !(next[0] == 0 && next[1] == 0))
            ++next;
        data = next + 2;
    }
}

struct WmiSession {
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> svc;
    bool ok = false;

    WmiSession(const wchar_t* ns) {
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))))
            return;
        BSTR path = SysAllocString(ns);
        HRESULT hr = locator->ConnectServer(path, nullptr, nullptr, nullptr,
                                            0, nullptr, nullptr, &svc);
        SysFreeString(path);
        if (FAILED(hr)) return;

        CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                          nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        ok = true;
    }

    ComPtr<IEnumWbemClassObject> query(const wchar_t* wql) const {
        if (!ok) return {};
        BSTR lang = SysAllocString(L"WQL");
        BSTR q = SysAllocString(wql);
        ComPtr<IEnumWbemClassObject> enumerator;
        HRESULT hr = svc->ExecQuery(lang, q,
                                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                    nullptr, &enumerator);
        SysFreeString(lang);
        SysFreeString(q);
        if (FAILED(hr)) return {};
        return enumerator;
    }
};

std::string wmi_string(IWbemClassObject* obj, const wchar_t* prop) {
    VARIANT v;
    VariantInit(&v);
    if (FAILED(obj->Get(prop, 0, &v, nullptr, nullptr))) return {};
    std::string result;
    if (v.vt == VT_BSTR && v.bstrVal)
        result = wide_to_narrow(v.bstrVal, static_cast<int>(SysStringLen(v.bstrVal)));
    VariantClear(&v);

    while (!result.empty() && (result.back() == ' ' || result.back() == '\0'))
        result.pop_back();
    return result;
}

std::int64_t wmi_int64(IWbemClassObject* obj, const wchar_t* prop) {
    VARIANT v;
    VariantInit(&v);
    if (FAILED(obj->Get(prop, 0, &v, nullptr, nullptr))) return 0;
    std::int64_t result = 0;
    if (v.vt == VT_BSTR && v.bstrVal)
        result = _wcstoi64(v.bstrVal, nullptr, 10);
    else if (v.vt == VT_I4)
        result = v.lVal;
    else if (v.vt == VT_UI4)
        result = v.ulVal;
    VariantClear(&v);
    return result;
}

void read_wmi_cimv2(HwidProfile& p) {
    WmiSession wmi(L"ROOT\\CIMV2");
    if (!wmi.ok) return;

    auto disk_enum = wmi.query(L"SELECT SerialNumber FROM Win32_DiskDrive");
    if (disk_enum) {
        ComPtr<IWbemClassObject> obj;
        ULONG ret = 0;
        if (SUCCEEDED(disk_enum->Next(WBEM_INFINITE, 1, &obj, &ret)) && ret > 0)
            p.disk_serial = wmi_string(obj.Get(), L"SerialNumber");
    }

    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
            ComPtr<IMMDevice> device;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
                ComPtr<IPropertyStore> props;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &pv))
                        && pv.vt == VT_LPWSTR && pv.pwszVal) {
                        p.sound_card = wide_to_narrow(pv.pwszVal);
                    }
                    PropVariantClear(&pv);
                }
            }
        }
    }
}

std::string format_storage_size(std::int64_t bytes) {
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1000.0) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%.1f TB", gb / 1024.0);
        return buf;
    }
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.0f GB", gb);
    return buf;
}

void read_storage(HwidProfile& p) {
    WmiSession wmi(L"ROOT\\Microsoft\\Windows\\Storage");
    if (!wmi.ok) {
        p.storage_ssds = 1;
        p.storage_ssd_size = "Unknown";
        return;
    }

    auto disk_enum = wmi.query(L"SELECT MediaType, Size FROM MSFT_PhysicalDisk");
    if (!disk_enum) {
        p.storage_ssds = 1;
        p.storage_ssd_size = "Unknown";
        return;
    }

    std::int64_t ssd_total = 0;
    std::int64_t hdd_total = 0;

    ComPtr<IWbemClassObject> obj;
    ULONG ret = 0;
    while (SUCCEEDED(disk_enum->Next(WBEM_INFINITE, 1, &obj, &ret)) && ret > 0) {
        VARIANT vtype;
        VariantInit(&vtype);
        if (SUCCEEDED(obj->Get(L"MediaType", 0, &vtype, nullptr, nullptr))) {
            int media = 0;
            if (vtype.vt == VT_I4) media = vtype.lVal;
            else if (vtype.vt == VT_UI4) media = static_cast<int>(vtype.ulVal);
            else if (vtype.vt == VT_I2) media = vtype.iVal;
            else if (vtype.vt == VT_UI2) media = vtype.uiVal;
            VariantClear(&vtype);

            std::int64_t sz = wmi_int64(obj.Get(), L"Size");

            if (media == 4) {
                ++p.storage_ssds;
                ssd_total += sz;
            } else if (media == 3) {
                ++p.storage_hdds;
                hdd_total += sz;
            } else {
                ++p.storage_ssds;
                ssd_total += sz;
            }
        }
        obj.Reset();
        ret = 0;
    }

    if (ssd_total > 0) p.storage_ssd_size = format_storage_size(ssd_total);
    if (hdd_total > 0) p.storage_hdd_size = format_storage_size(hdd_total);
    if (p.storage_ssds == 0 && p.storage_hdds == 0) {
        p.storage_ssds = 1;
        p.storage_ssd_size = "Unknown";
    }
}

}  // namespace

HwidProfile read_real_hardware() {
    HwidProfile p;

    p.machine_guid = read_machine_guid();
    p.mac_address = read_mac_address();
    p.pc_name = read_pc_name();
    read_gpu(p);
    p.ram_mb = read_total_ram_mb();
    read_monitor(p);
    p.display_model = read_display_model();
    read_board(p);

    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool did_init = SUCCEEDED(co);
    if (did_init || co == RPC_E_CHANGED_MODE) {
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                            RPC_C_AUTHN_LEVEL_DEFAULT,
                            RPC_C_IMP_LEVEL_IMPERSONATE,
                            nullptr, EOAC_NONE, nullptr);

        read_wmi_cimv2(p);
        read_storage(p);
    }
    if (did_init) CoUninitialize();

    return p;
}

}  // namespace sam::core::hwid
