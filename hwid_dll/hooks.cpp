#include "hooks.hpp"
#include "ipc_reader.hpp"
#include "scanner.hpp"

#include <windows.h>
#include <xorstr.hpp>
#include <MinHook.h>
#include <wbemidl.h>
#include <dxgi.h>
#include <d3d9.h>
#include <string>
#include <cstring>
#include <algorithm>
#include <mutex>

namespace {
constexpr uint32_t kMachineGuid = 1u << 0;
constexpr uint32_t kMac         = 1u << 1;
constexpr uint32_t kDiskSerial  = 1u << 2;
constexpr uint32_t kPcName      = 1u << 3;
constexpr uint32_t kGpu         = 1u << 4;
constexpr uint32_t kMotherboard = 1u << 5;
constexpr uint32_t kRam         = 1u << 6;
constexpr uint32_t kMonitor     = 1u << 7;
constexpr uint32_t kStorage     = 1u << 8;
constexpr uint32_t kSoundCard   = 1u << 9;
}

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d9.lib")

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

static uint32_t parse_hex(const std::string& s) {
    return static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 16));
}

static bool iequals(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

static void safe_wcscpy(wchar_t* dst, size_t dst_chars, const std::wstring& src) {
    size_t copy_len = (std::min)(src.size(), dst_chars - 1);
    memcpy(dst, src.data(), copy_len * sizeof(wchar_t));
    dst[copy_len] = L'\0';
}

static bool parse_mac_bytes(const std::string& mac_str, uint8_t out[6]) {
    if (mac_str.size() < 12) return false;
    std::string clean;
    for (char c : mac_str) {
        if (c != ':' && c != '-')
            clean += c;
    }
    if (clean.size() < 12) return false;
    for (int i = 0; i < 6; ++i) {
        out[i] = static_cast<uint8_t>(strtoul(clean.substr(i * 2, 2).c_str(), nullptr, 16));
    }
    return true;
}

static uint64_t parse_size_bytes(const std::string& s) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        v = v * 10 + static_cast<uint64_t>(s[i++] - '0');
    if (i < s.size()) {
        char u = static_cast<char>(toupper(static_cast<unsigned char>(s[i])));
        if      (u == 'T') v *= 1000000000000ULL;
        else if (u == 'G') v *= 1000000000ULL;
        else if (u == 'M') v *= 1000000ULL;
    }
    return v;
}

template <typename T>
static void create_and_enable(void* target, void* detour, T** original) {
    MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
    MH_EnableHook(target);
}

static void wmi_put_str(IWbemClassObject* obj, const wchar_t* prop, const std::string& value) {
    if (value.empty()) return;
    auto ws = widen(value);
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BSTR;
    var.bstrVal = SysAllocString(ws.c_str());
    obj->Put(prop, 0, &var, 0);
    VariantClear(&var);
}

static void wmi_put_u32(IWbemClassObject* obj, const wchar_t* prop, uint32_t value) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_I4;
    var.lVal = static_cast<long>(value);
    obj->Put(prop, 0, &var, 0);
    VariantClear(&var);
}

static void wmi_put_u64(IWbemClassObject* obj, const wchar_t* prop, uint64_t value) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BSTR;
    auto ws = std::to_wstring(value);
    var.bstrVal = SysAllocString(ws.c_str());
    obj->Put(prop, 0, &var, 0);
    VariantClear(&var);
}

static std::wstring wmi_get_class(IWbemClassObject* obj) {
    VARIANT var;
    VariantInit(&var);
    wchar_t prop[] = { '_','_','C','L','A','S','S',0 };
    if (SUCCEEDED(obj->Get(prop, 0, &var, nullptr, nullptr)) && var.vt == VT_BSTR) {
        std::wstring cls(var.bstrVal);
        VariantClear(&var);
        return cls;
    }
    VariantClear(&var);
    return {};
}

using FnNext = HRESULT(STDMETHODCALLTYPE*)(IEnumWbemClassObject*, long, ULONG, IWbemClassObject**, ULONG*);
static FnNext s_orig_next = nullptr;

static HRESULT STDMETHODCALLTYPE hk_next(IEnumWbemClassObject* self, long timeout,
                                          ULONG count, IWbemClassObject** objects, ULONG* returned) {
    HRESULT hr = s_orig_next(self, timeout, count, objects, returned);
    if (FAILED(hr) || !returned || *returned == 0) return hr;

    const auto& prof = ipc::get_profile();

    for (ULONG i = 0; i < *returned; ++i) {
        auto* obj = objects[i];
        if (!obj) continue;

        auto cls = wmi_get_class(obj);
        if (cls.empty()) continue;

        if (iequals(cls.c_str(), xorstr_(L"Win32_BaseBoard"))) {
            if (prof.spoof_mask & kMotherboard) {
                wmi_put_str(obj, xorstr_(L"Manufacturer"), prof.board_manufacturer);
                wmi_put_str(obj, xorstr_(L"Product"), prof.board_model);
                wmi_put_str(obj, xorstr_(L"Model"), prof.board_model);
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"Win32_VideoController"))) {
            if (prof.spoof_mask & kGpu) {
                wmi_put_str(obj, xorstr_(L"Name"), prof.gpu_name);
                wmi_put_str(obj, xorstr_(L"Caption"), prof.gpu_name);
                wmi_put_str(obj, xorstr_(L"Description"), prof.gpu_name);
                if (prof.gpu_vram_mb > 0) {
                    uint64_t vram_bytes = static_cast<uint64_t>(prof.gpu_vram_mb) * 1024ull * 1024ull;
                    wmi_put_u32(obj, xorstr_(L"AdapterRAM"),
                        vram_bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(vram_bytes));
                }
                if (!prof.gpu_vendor_id.empty() || !prof.gpu_device_id.empty()) {
                    char pnp_buf[128];
                    snprintf(pnp_buf, sizeof(pnp_buf), "%s%s%s%s%s",
                             xorstr_("PCI\\VEN_"), prof.gpu_vendor_id.c_str(),
                             xorstr_("&DEV_"), prof.gpu_device_id.c_str(),
                             xorstr_("&SUBSYS_00000000&REV_A1"));
                    wmi_put_str(obj, xorstr_(L"PNPDeviceID"), std::string(pnp_buf));
                }
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"Win32_PhysicalMemory"))) {
            if (prof.spoof_mask & kRam) {
                if (prof.ram_mb > 0) {
                    uint64_t per_stick = (static_cast<uint64_t>(prof.ram_mb) * 1024ull * 1024ull) / *returned;
                    wmi_put_u64(obj, xorstr_(L"Capacity"), per_stick);
                }
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"Win32_SoundDevice"))) {
            if (prof.spoof_mask & kSoundCard) {
                wmi_put_str(obj, xorstr_(L"Name"), prof.sound_card);
                wmi_put_str(obj, xorstr_(L"Caption"), prof.sound_card);
                wmi_put_str(obj, xorstr_(L"Description"), prof.sound_card);
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"Win32_DesktopMonitor"))) {
            if (prof.spoof_mask & kMonitor) {
                wmi_put_str(obj, xorstr_(L"Name"), prof.display_model);
                wmi_put_str(obj, xorstr_(L"Caption"), prof.display_model);
                wmi_put_str(obj, xorstr_(L"Description"), prof.display_model);
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"Win32_DiskDrive"))) {
            if (prof.spoof_mask & kStorage) {
                wmi_put_str(obj, xorstr_(L"SerialNumber"), prof.disk_serial);
                bool is_ssd = (static_cast<int>(i) < prof.storage_ssds);
                uint64_t sz = is_ssd
                    ? parse_size_bytes(prof.storage_ssd_size)
                    : parse_size_bytes(prof.storage_hdd_size);
                if (sz > 0) wmi_put_u64(obj, xorstr_(L"Size"), sz);
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"Win32_PhysicalMedia"))) {
            if (prof.spoof_mask & kStorage) {
                wmi_put_str(obj, xorstr_(L"SerialNumber"), prof.disk_serial);
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"MSFT_PhysicalDisk"))) {
            if (prof.spoof_mask & kStorage) {
                wmi_put_str(obj, xorstr_(L"SerialNumber"), prof.disk_serial);
                bool is_ssd = (static_cast<int>(i) < prof.storage_ssds);
                wmi_put_u32(obj, xorstr_(L"MediaType"), is_ssd ? 4u : 3u);
                uint64_t sz = is_ssd
                    ? parse_size_bytes(prof.storage_ssd_size)
                    : parse_size_bytes(prof.storage_hdd_size);
                if (sz > 0) wmi_put_u64(obj, xorstr_(L"Size"), sz);
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"Win32_ComputerSystem"))) {
            if (prof.spoof_mask & kMotherboard) {
                wmi_put_str(obj, xorstr_(L"Model"), prof.board_model);
                wmi_put_str(obj, xorstr_(L"Manufacturer"), prof.board_manufacturer);
            }
        }
        else if (iequals(cls.c_str(), xorstr_(L"Win32_ComputerSystemProduct"))) {
            if (prof.spoof_mask & kMotherboard) {
                wmi_put_str(obj, xorstr_(L"Name"), prof.board_model);
                wmi_put_str(obj, xorstr_(L"Vendor"), prof.board_manufacturer);
            }
        }
    }
    return hr;
}

using FnExecQuery = HRESULT(STDMETHODCALLTYPE*)(IWbemServices*, const BSTR, const BSTR, long,
                                                  IWbemContext*, IEnumWbemClassObject**);
static FnExecQuery s_orig_exec_query = nullptr;
static std::mutex  s_next_hook_mtx;

static HRESULT STDMETHODCALLTYPE hk_exec_query(IWbemServices* self, const BSTR lang,
                                                 const BSTR query, long flags,
                                                 IWbemContext* ctx, IEnumWbemClassObject** enumerator) {
    HRESULT hr = s_orig_exec_query(self, lang, query, flags, ctx, enumerator);
    if (SUCCEEDED(hr) && enumerator && *enumerator) {
        std::lock_guard<std::mutex> lock(s_next_hook_mtx);
        auto** vtable = *reinterpret_cast<void***>(*enumerator);
        if (vtable[4] != hk_next) {
            s_orig_next = reinterpret_cast<FnNext>(
                scanner::vtable_swap(vtable, 4, reinterpret_cast<void*>(hk_next)));
        }
    }
    return hr;
}

using FnConnectServer = HRESULT(STDMETHODCALLTYPE*)(IWbemLocator*, BSTR, BSTR, BSTR, BSTR,
                                                      LONG, BSTR, IWbemContext*, IWbemServices**);
static FnConnectServer s_orig_connect = nullptr;
static std::mutex      s_exec_hook_mtx;

static HRESULT STDMETHODCALLTYPE hk_connect_server(IWbemLocator* self, BSTR ns,
                                                     BSTR user, BSTR pass, BSTR locale,
                                                     LONG security_flags, BSTR authority,
                                                     IWbemContext* ctx, IWbemServices** svc) {
    HRESULT hr = s_orig_connect(self, ns, user, pass, locale, security_flags, authority, ctx, svc);
    if (SUCCEEDED(hr) && svc && *svc) {
        std::lock_guard<std::mutex> lock(s_exec_hook_mtx);
        auto** vtable = *reinterpret_cast<void***>(*svc);
        if (vtable[20] != hk_exec_query) {
            s_orig_exec_query = reinterpret_cast<FnExecQuery>(
                scanner::vtable_swap(vtable, 20, reinterpret_cast<void*>(hk_exec_query)));
        }
    }
    return hr;
}

static const GUID kClsidMMDevEnum =
    {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};

static const struct { GUID fmtid; DWORD pid; } kPkeyFriendly =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

using FnPSGetValue = HRESULT(STDMETHODCALLTYPE*)(void*, const void*, PROPVARIANT*);
static FnPSGetValue s_orig_ps_getvalue = nullptr;

static HRESULT STDMETHODCALLTYPE hk_ps_getvalue(void* self, const void* key, PROPVARIANT* pv) {
    HRESULT hr = s_orig_ps_getvalue(self, key, pv);
    if (FAILED(hr) || !key || !pv) return hr;

    const auto& prof = ipc::get_profile();
    if (!(prof.spoof_mask & kSoundCard) || prof.sound_card.empty()) return hr;

    struct PK { GUID fmtid; DWORD pid; };
    auto* pk = static_cast<const PK*>(key);
    if (!IsEqualGUID(pk->fmtid, kPkeyFriendly.fmtid) || pk->pid != kPkeyFriendly.pid)
        return hr;

    if (pv->vt != VT_LPWSTR || !pv->pwszVal) return hr;

    auto wide = widen(prof.sound_card);
    size_t orig_len = wcslen(pv->pwszVal);
    size_t new_len = wide.size();
    if (new_len <= orig_len) {
        memcpy(pv->pwszVal, wide.c_str(), (new_len + 1) * sizeof(wchar_t));
    } else {
        CoTaskMemFree(pv->pwszVal);
        pv->pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc((new_len + 1) * sizeof(wchar_t)));
        if (pv->pwszVal)
            memcpy(pv->pwszVal, wide.c_str(), (new_len + 1) * sizeof(wchar_t));
    }

    return hr;
}

using FnOpenPropStore = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void**);
static FnOpenPropStore s_orig_open_props = nullptr;

static HRESULT STDMETHODCALLTYPE hk_open_props(void* self, DWORD access, void** ppStore) {
    HRESULT hr = s_orig_open_props(self, access, ppStore);
    if (SUCCEEDED(hr) && ppStore && *ppStore && !s_orig_ps_getvalue) {
        auto** vt = *reinterpret_cast<void***>(*ppStore);
        create_and_enable(vt[5], reinterpret_cast<void*>(hk_ps_getvalue), &s_orig_ps_getvalue);
    }
    return hr;
}

using FnGetDefaultEP = HRESULT(STDMETHODCALLTYPE*)(void*, int, int, void**);
static FnGetDefaultEP s_orig_get_default_ep = nullptr;

static HRESULT STDMETHODCALLTYPE hk_get_default_ep(void* self, int flow, int role, void** ppDev) {
    HRESULT hr = s_orig_get_default_ep(self, flow, role, ppDev);
    if (SUCCEEDED(hr) && ppDev && *ppDev && !s_orig_open_props) {
        auto** vt = *reinterpret_cast<void***>(*ppDev);
        create_and_enable(vt[4], reinterpret_cast<void*>(hk_open_props), &s_orig_open_props);
    }
    return hr;
}

using FnCoCreateInstance = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static FnCoCreateInstance s_orig_cocreate = nullptr;

static HRESULT WINAPI hk_cocreate(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx,
                                   REFIID iid, LPVOID* ppv) {
    HRESULT hr = s_orig_cocreate(clsid, outer, ctx, iid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (IsEqualCLSID(clsid, CLSID_WbemLocator)) {
            if (!s_orig_connect) {
                auto** vtable = *reinterpret_cast<void***>(*ppv);
                s_orig_connect = reinterpret_cast<FnConnectServer>(
                    scanner::vtable_swap(vtable, 3, reinterpret_cast<void*>(hk_connect_server)));
            }
        }
        else if (IsEqualCLSID(clsid, kClsidMMDevEnum)) {
            if (!s_orig_get_default_ep) {
                auto** vtable = *reinterpret_cast<void***>(*ppv);
                create_and_enable(vtable[4],
                    reinterpret_cast<void*>(hk_get_default_ep), &s_orig_get_default_ep);
            }
        }
    }
    return hr;
}

static void spoof_adapter_desc(DXGI_ADAPTER_DESC* desc) {
    const auto& prof = ipc::get_profile();
    if (!(prof.spoof_mask & kGpu)) return;
    if (prof.gpu_name.empty()) return;
    auto wide_name = widen(prof.gpu_name);
    safe_wcscpy(desc->Description, 128, wide_name);
    if (!prof.gpu_vendor_id.empty())
        desc->VendorId = parse_hex(prof.gpu_vendor_id);
    if (!prof.gpu_device_id.empty())
        desc->DeviceId = parse_hex(prof.gpu_device_id);
    if (prof.gpu_vram_mb > 0)
        desc->DedicatedVideoMemory = static_cast<SIZE_T>(prof.gpu_vram_mb) * 1024ull * 1024ull;
}

using FnGetDesc = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*, DXGI_ADAPTER_DESC*);
static FnGetDesc s_orig_get_desc = nullptr;

static HRESULT STDMETHODCALLTYPE hk_get_desc(IDXGIAdapter* self, DXGI_ADAPTER_DESC* desc) {

    HRESULT hr = s_orig_get_desc(self, desc);
    if (SUCCEEDED(hr)) spoof_adapter_desc(desc);
    return hr;
}

using FnGetDesc1 = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
static FnGetDesc1 s_orig_get_desc1 = nullptr;

static HRESULT STDMETHODCALLTYPE hk_get_desc1(IDXGIAdapter1* self, DXGI_ADAPTER_DESC1* desc) {
    HRESULT hr = s_orig_get_desc1(self, desc);
    if (SUCCEEDED(hr)) spoof_adapter_desc(reinterpret_cast<DXGI_ADAPTER_DESC*>(desc));
    return hr;
}

using FnEnumAdapters = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, UINT, IDXGIAdapter**);
static FnEnumAdapters s_orig_enum_adapters = nullptr;
static std::mutex     s_adapter_hook_mtx;

static HRESULT STDMETHODCALLTYPE hk_enum_adapters(IDXGIFactory* self, UINT index, IDXGIAdapter** adapter) {
    HRESULT hr = s_orig_enum_adapters(self, index, adapter);
    if (SUCCEEDED(hr) && index == 0 && adapter && *adapter) {
        std::lock_guard<std::mutex> lock(s_adapter_hook_mtx);
        auto** vtable = *reinterpret_cast<void***>(*adapter);

        if (vtable[8] != hk_get_desc) {
            s_orig_get_desc = reinterpret_cast<FnGetDesc>(
                scanner::vtable_swap(vtable, 8, reinterpret_cast<void*>(hk_get_desc)));
        }

        IDXGIAdapter1* adapter1 = nullptr;
        if (SUCCEEDED((*adapter)->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter1)))) {
            auto** vt1 = *reinterpret_cast<void***>(adapter1);
            if (vt1[10] != hk_get_desc1) {
                s_orig_get_desc1 = reinterpret_cast<FnGetDesc1>(
                    scanner::vtable_swap(vt1, 10, reinterpret_cast<void*>(hk_get_desc1)));
            }
            adapter1->Release();
        }
    }
    return hr;
}

using FnCreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
static FnCreateDXGIFactory s_orig_create_factory  = nullptr;
static FnCreateDXGIFactory s_orig_create_factory1 = nullptr;

using FnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
static FnCreateDXGIFactory2 s_orig_create_factory2 = nullptr;

static void hook_factory_vtable(void* factory_ptr) {
    if (!factory_ptr) return;
    std::lock_guard<std::mutex> lock(s_adapter_hook_mtx);
    auto** vtable = *reinterpret_cast<void***>(factory_ptr);
    if (vtable[7] != hk_enum_adapters) {
        s_orig_enum_adapters = reinterpret_cast<FnEnumAdapters>(
            scanner::vtable_swap(vtable, 7, reinterpret_cast<void*>(hk_enum_adapters)));
    }
}

static HRESULT WINAPI hk_create_factory(REFIID iid, void** ppFactory) {
    HRESULT hr = s_orig_create_factory(iid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory) hook_factory_vtable(*ppFactory);
    return hr;
}

static HRESULT WINAPI hk_create_factory1(REFIID iid, void** ppFactory) {
    HRESULT hr = s_orig_create_factory1(iid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory) hook_factory_vtable(*ppFactory);
    return hr;
}

static HRESULT WINAPI hk_create_factory2(UINT flags, REFIID iid, void** ppFactory) {
    HRESULT hr = s_orig_create_factory2(flags, iid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory) hook_factory_vtable(*ppFactory);
    return hr;
}

static void smbios_set_string(uint8_t* str_section, size_t sec_size,
                               uint8_t string_index, const std::string& replacement) {
    if (replacement.empty() || string_index == 0) return;
    uint8_t* end = str_section + sec_size;
    uint8_t* s = str_section;
    for (uint8_t cur = 1; cur < string_index; ++cur) {
        while (s < end && *s) ++s;
        if (s < end) ++s;
    }
    if (s >= end) return;
    size_t old_len = strlen(reinterpret_cast<char*>(s));
    size_t copy_len = (std::min)(replacement.size(), old_len);
    memcpy(s, replacement.data(), copy_len);
    if (copy_len < old_len)
        memset(s + copy_len, ' ', old_len - copy_len);
}

using FnGetSystemFirmwareTable = UINT(WINAPI*)(DWORD, DWORD, PVOID, DWORD);
static FnGetSystemFirmwareTable s_orig_firmware = nullptr;

static UINT WINAPI hk_firmware(DWORD sig, DWORD id, PVOID buf, DWORD size) {

    UINT result = s_orig_firmware(sig, id, buf, size);
    if (sig != 'RSMB' || !buf || result <= 8) return result;

    const auto& prof = ipc::get_profile();
    if (!(prof.spoof_mask & kMotherboard)) return result;
    if (prof.board_model.empty()) return result;

    auto* raw = static_cast<uint8_t*>(buf);
    uint8_t* table = raw + 8;
    DWORD table_len = result - 8;
    uint8_t* table_end = table + table_len;

    for (uint8_t* p = table; p + 4 <= table_end; ) {
        uint8_t type   = p[0];
        uint8_t hdrlen = p[1];
        if (type == 127) break;
        if (p + hdrlen > table_end) break;

        uint8_t* str0 = p + hdrlen;
        uint8_t* q = str0;
        while (q + 1 < table_end && !(q[0] == 0 && q[1] == 0)) ++q;
        uint8_t* struct_end = (q + 2 <= table_end) ? q + 2 : table_end;
        size_t sec_sz = static_cast<size_t>(struct_end - str0);

        if ((type == 1 || type == 2) && hdrlen > 5) {
            uint8_t mfr_idx  = p[4];
            uint8_t prod_idx = p[5];
            if (mfr_idx)
                smbios_set_string(str0, sec_sz, mfr_idx, prof.board_manufacturer);
            if (prod_idx)
                smbios_set_string(str0, sec_sz, prod_idx, prof.board_model);
        }

        p = struct_end;
    }
    return result;
}

using FnGlobalMemoryStatusEx = BOOL(WINAPI*)(LPMEMORYSTATUSEX);
static FnGlobalMemoryStatusEx s_orig_memstatus = nullptr;

static BOOL WINAPI hk_memstatus(LPMEMORYSTATUSEX ms) {
    BOOL ok = s_orig_memstatus(ms);
    if (ok && (ipc::get_profile().spoof_mask & kRam) && ipc::get_profile().ram_mb > 0) {
        uint64_t bytes = static_cast<uint64_t>(ipc::get_profile().ram_mb) * 1024ull * 1024ull;
        ms->ullTotalPhys = bytes;
        if (ms->ullAvailPhys > bytes)
            ms->ullAvailPhys = bytes / 4;
    }
    return ok;
}

using FnGetSystemMetrics = int(WINAPI*)(int);
static FnGetSystemMetrics s_orig_metrics = nullptr;

static int WINAPI hk_metrics(int index) {
    const auto& prof = ipc::get_profile();
    if ((prof.spoof_mask & kMonitor) && prof.monitor_width > 0) {
        if (index == SM_CXSCREEN)        return prof.monitor_width;
        if (index == SM_CYSCREEN)        return prof.monitor_height;
        if (index == SM_CXVIRTUALSCREEN) return prof.monitor_width;
        if (index == SM_CYVIRTUALSCREEN) return prof.monitor_height;
        if (index == SM_XVIRTUALSCREEN)  return 0;
        if (index == SM_YVIRTUALSCREEN)  return 0;
        if (index == SM_CMONITORS)       return 1;
    }
    return s_orig_metrics(index);
}

using FnGetDeviceCaps = int(WINAPI*)(HDC, int);
static FnGetDeviceCaps s_orig_devcaps = nullptr;

static int WINAPI hk_devcaps(HDC hdc, int cap) {
    if (cap == VREFRESH && (ipc::get_profile().spoof_mask & kMonitor) && ipc::get_profile().monitor_refresh > 0)
        return ipc::get_profile().monitor_refresh;
    return s_orig_devcaps(hdc, cap);
}

using FnEnumDisplaySettingsW = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
static FnEnumDisplaySettingsW s_orig_enum_display = nullptr;

static BOOL WINAPI hk_enum_display(LPCWSTR device, DWORD mode, DEVMODEW* dm) {
    BOOL ok = s_orig_enum_display(device, mode, dm);
    if (ok && dm && (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS)
        && (ipc::get_profile().spoof_mask & kMonitor)) {
        const auto& prof = ipc::get_profile();
        if (prof.monitor_width > 0)   dm->dmPelsWidth  = prof.monitor_width;
        if (prof.monitor_height > 0)  dm->dmPelsHeight = prof.monitor_height;
        if (prof.monitor_refresh > 0) dm->dmDisplayFrequency = prof.monitor_refresh;
    }
    return ok;
}

using FnEnumDisplaySettingsExW = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
static FnEnumDisplaySettingsExW s_orig_enum_display_ex = nullptr;

static BOOL WINAPI hk_enum_display_ex(LPCWSTR device, DWORD mode, DEVMODEW* dm, DWORD flags) {
    BOOL ok = s_orig_enum_display_ex(device, mode, dm, flags);
    if (ok && dm && (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS)
        && (ipc::get_profile().spoof_mask & kMonitor)) {
        const auto& prof = ipc::get_profile();
        if (prof.monitor_width > 0)   dm->dmPelsWidth  = prof.monitor_width;
        if (prof.monitor_height > 0)  dm->dmPelsHeight = prof.monitor_height;
        if (prof.monitor_refresh > 0) dm->dmDisplayFrequency = prof.monitor_refresh;
    }
    return ok;
}

using FnEnumDisplayDevicesW = BOOL(WINAPI*)(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);
static FnEnumDisplayDevicesW s_orig_enum_devices = nullptr;

static BOOL WINAPI hk_enum_devices(LPCWSTR device, DWORD devnum, PDISPLAY_DEVICEW info, DWORD flags) {
    BOOL ok = s_orig_enum_devices(device, devnum, info, flags);
    if (ok && devnum == 0 && flags == 0 && info && (ipc::get_profile().spoof_mask & kMonitor)) {
        auto wide = widen(ipc::get_profile().display_model);
        if (!wide.empty())
            safe_wcscpy(info->DeviceString, 128, wide);
    }
    return ok;
}

using FnGetAdapterCount = UINT(STDMETHODCALLTYPE*)(IDirect3D9*);
static FnGetAdapterCount s_orig_adapter_count = nullptr;

static UINT STDMETHODCALLTYPE hk_adapter_count(IDirect3D9* self) {
    if (!(ipc::get_profile().spoof_mask & kGpu))
        return s_orig_adapter_count(self);
    return 1;
}

using FnGetAdapterDisplayMode = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDISPLAYMODE*);
static FnGetAdapterDisplayMode s_orig_adapter_mode = nullptr;

static HRESULT STDMETHODCALLTYPE hk_adapter_mode(IDirect3D9* self, UINT adapter, D3DDISPLAYMODE* mode) {
    HRESULT hr = s_orig_adapter_mode(self, adapter, mode);
    if (SUCCEEDED(hr) && mode && (ipc::get_profile().spoof_mask & kMonitor)) {
        const auto& prof = ipc::get_profile();
        if (prof.monitor_width > 0)  mode->Width  = prof.monitor_width;
        if (prof.monitor_height > 0) mode->Height = prof.monitor_height;
        if (prof.monitor_refresh > 0) mode->RefreshRate = prof.monitor_refresh;
    }
    return hr;
}

static void hook_d3d9() {
    HMODULE d3d9mod = GetModuleHandleA(xorstr_("d3d9.dll"));
    if (!d3d9mod) d3d9mod = LoadLibraryA(xorstr_("d3d9.dll"));
    if (!d3d9mod) return;

    auto pDirect3DCreate9 = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(
        GetProcAddress(d3d9mod, xorstr_("Direct3DCreate9")));
    if (!pDirect3DCreate9) return;

    IDirect3D9* d3d = pDirect3DCreate9(32);
    if (!d3d) return;

    auto** vtable = *reinterpret_cast<void***>(d3d);

    s_orig_adapter_count = reinterpret_cast<FnGetAdapterCount>(
        scanner::vtable_swap(vtable, 4, reinterpret_cast<void*>(hk_adapter_count)));
    s_orig_adapter_mode = reinterpret_cast<FnGetAdapterDisplayMode>(
        scanner::vtable_swap(vtable, 8, reinterpret_cast<void*>(hk_adapter_mode)));

    d3d->Release();
}

using FnMachineGuid   = char(__fastcall*)(unsigned char* buf, DWORD size);
using FnMacAddress    = DWORD(__fastcall*)(unsigned long long* a1);
using FnMacAddressTwo = __int64(__fastcall*)(unsigned __int64* a1, char a2);
using FnDiskSerial    = bool(__fastcall*)(unsigned char* buf, int a2);

static FnMachineGuid   s_orig_machine_guid   = nullptr;
static FnMacAddress    s_orig_mac_address     = nullptr;
static FnMacAddressTwo s_orig_mac_address_two = nullptr;
static FnDiskSerial    s_orig_disk_serial     = nullptr;

static char __fastcall hk_machine_guid(unsigned char* buf, DWORD size) {
    char result = s_orig_machine_guid(buf, size);
    const auto& prof = ipc::get_profile();
    if (result != 0 && (prof.spoof_mask & kMachineGuid) && !prof.machine_guid.empty()) {
        strcpy_s(reinterpret_cast<char*>(buf), size, prof.machine_guid.c_str());
    }
    return result;
}

static DWORD __fastcall hk_mac_address(unsigned long long* a1) {
    DWORD result = s_orig_mac_address(a1);
    const auto& prof = ipc::get_profile();
    if (a1 != nullptr && (prof.spoof_mask & kMac) && !prof.mac_address.empty()) {
        uint8_t mac[6];
        if (parse_mac_bytes(prof.mac_address, mac)) {
            *a1 = 0;
            std::memcpy(a1, mac, 6);
        }
    }
    return result;
}

static __int64 __fastcall hk_mac_address_two(unsigned __int64* a1, char a2) {
    __int64 result = s_orig_mac_address_two(a1, a2);
    const auto& prof = ipc::get_profile();
    if (a1 != nullptr && (prof.spoof_mask & kMac) && !prof.mac_address.empty()) {
        uint8_t mac[6];
        if (parse_mac_bytes(prof.mac_address, mac)) {
            *a1 = 0;
            std::memcpy(a1, mac, 6);
        }
    }
    return result;
}

static bool __fastcall hk_disk_serial(unsigned char* buf, int a2) {
    bool result = s_orig_disk_serial(buf, a2);
    const auto& prof = ipc::get_profile();
    if (result && (prof.spoof_mask & kDiskSerial) && !prof.disk_serial.empty()) {
        strcpy_s(reinterpret_cast<char*>(buf), 256, prof.disk_serial.c_str());
    }
    return result;
}

static void hook_steamclient_inner(const char* sc_name);

static void hook_steamclient() {
    HMODULE mod = GetModuleHandleA(xorstr_("steamclient64.dll"));
    if (!mod) return;

    char sc_name[32];
    strncpy_s(sc_name, sizeof(sc_name), xorstr_("steamclient64.dll"), _TRUNCATE);
    hook_steamclient_inner(sc_name);
}

static bool try_hook_pattern(const char* module, const char* pattern,
                              void* hook_fn, void** orig_fn) {
    auto call_site = scanner::find_pattern(module, pattern);
    if (!call_site) return false;
    auto target = scanner::resolve_call(call_site);
    if (!target) return false;
    MH_CreateHook(reinterpret_cast<void*>(target), hook_fn, orig_fn);
    MH_EnableHook(reinterpret_cast<void*>(target));
    return true;
}

static void hook_steamclient_inner(const char* sc_name) {
    try_hook_pattern(sc_name,
        xorstr_("E8 ? ? ? ? 0F B6 4D ? 84 C0"),
        reinterpret_cast<void*>(hk_machine_guid),
        reinterpret_cast<void**>(&s_orig_machine_guid));

    try_hook_pattern(sc_name,
        xorstr_("E8 ? ? ? ? 4C 8B A4 24 ? ? ? ? 85 C0"),
        reinterpret_cast<void*>(hk_mac_address),
        reinterpret_cast<void**>(&s_orig_mac_address));

    try_hook_pattern(sc_name,
        xorstr_("E8 ? ? ? ? 85 C0 74 ? 48 85 DB 74 ? 48 83 7C 24"),
        reinterpret_cast<void*>(hk_mac_address_two),
        reinterpret_cast<void**>(&s_orig_mac_address_two));

    try_hook_pattern(sc_name,
        xorstr_("E8 ? ? ? ? 48 8B B4 24 ? ? ? ? 48 8B 9C 24 ? ? ? ? 84 C0"),
        reinterpret_cast<void*>(hk_disk_serial),
        reinterpret_cast<void**>(&s_orig_disk_serial));
}

using FnGetLocalHostname = const char*(__fastcall*)();
static FnGetLocalHostname s_orig_hostname = nullptr;

static const char* __fastcall hk_hostname() {
    const auto& prof = ipc::get_profile();
    if ((prof.spoof_mask & kPcName) && !prof.pc_name.empty())
        return prof.pc_name.c_str();
    return s_orig_hostname();
}

static void hook_tier0() {
    HMODULE mod = GetModuleHandleA(xorstr_("tier0_s64.dll"));
    if (!mod) return;

    auto* proc = GetProcAddress(mod, xorstr_("GetLocalHostname"));
    if (proc) {
        create_and_enable(reinterpret_cast<void*>(proc),
                           reinterpret_cast<void*>(hk_hostname), &s_orig_hostname);
    }
}

struct UNICODE_STRING_NT {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct LDR_DLL_NOTIFICATION_DATA {
    ULONG                    Flags;
    const UNICODE_STRING_NT* FullDllName;
    const UNICODE_STRING_NT* BaseDllName;
    void*                    DllBase;
    ULONG                    SizeOfImage;
};

using FnLdrRegisterDllNotification = LONG(NTAPI*)(ULONG, void*, void*, void**);
using FnLdrCallback = void(NTAPI*)(ULONG reason, const LDR_DLL_NOTIFICATION_DATA* data, void* ctx);

static bool dll_name_matches(const UNICODE_STRING_NT* name, const wchar_t* target) {
    if (!name || !name->Buffer) return false;
    size_t target_len = wcslen(target);
    size_t name_len = name->Length / sizeof(wchar_t);
    if (name_len != target_len) return false;
    return _wcsnicmp(name->Buffer, target, target_len) == 0;
}

static void NTAPI on_dll_loaded(ULONG reason, const LDR_DLL_NOTIFICATION_DATA* data, void* ctx) {
    (void)ctx;
    if (reason != 1) return;

    if (dll_name_matches(data->BaseDllName, xorstr_(L"steamclient64.dll")))
        hook_steamclient();
    else if (dll_name_matches(data->BaseDllName, xorstr_(L"tier0_s64.dll")))
        hook_tier0();
}

void hooks::init() {
    MH_Initialize();

    auto* cocreate = GetProcAddress(GetModuleHandleA(xorstr_("ole32.dll")), xorstr_("CoCreateInstance"));
    if (cocreate)
        create_and_enable(reinterpret_cast<void*>(cocreate),
                           reinterpret_cast<void*>(hk_cocreate), &s_orig_cocreate);

    HMODULE dxgi = GetModuleHandleA(xorstr_("dxgi.dll"));
    if (!dxgi) dxgi = LoadLibraryA(xorstr_("dxgi.dll"));
    if (dxgi) {
        auto* cf  = GetProcAddress(dxgi, xorstr_("CreateDXGIFactory"));
        auto* cf1 = GetProcAddress(dxgi, xorstr_("CreateDXGIFactory1"));
        auto* cf2 = GetProcAddress(dxgi, xorstr_("CreateDXGIFactory2"));

        if (cf)  create_and_enable(reinterpret_cast<void*>(cf),
                                    reinterpret_cast<void*>(hk_create_factory), &s_orig_create_factory);
        if (cf1) create_and_enable(reinterpret_cast<void*>(cf1),
                                    reinterpret_cast<void*>(hk_create_factory1), &s_orig_create_factory1);
        if (cf2) create_and_enable(reinterpret_cast<void*>(cf2),
                                    reinterpret_cast<void*>(hk_create_factory2), &s_orig_create_factory2);
    }

    hook_d3d9();

    HMODULE kb = GetModuleHandleA(xorstr_("kernelbase.dll"));
    if (!kb) kb = GetModuleHandleA(xorstr_("kernel32.dll"));
    if (kb) {
        auto* fw = GetProcAddress(kb, xorstr_("GetSystemFirmwareTable"));
        if (fw)
            create_and_enable(reinterpret_cast<void*>(fw),
                               reinterpret_cast<void*>(hk_firmware), &s_orig_firmware);
    }

    auto* mem = GetProcAddress(GetModuleHandleA(xorstr_("kernel32.dll")),
                                xorstr_("GlobalMemoryStatusEx"));
    if (mem)
        create_and_enable(reinterpret_cast<void*>(mem),
                           reinterpret_cast<void*>(hk_memstatus), &s_orig_memstatus);

    auto* user32 = GetModuleHandleA(xorstr_("user32.dll"));
    if (user32) {
        auto* sm = GetProcAddress(user32, xorstr_("GetSystemMetrics"));
        if (sm) create_and_enable(reinterpret_cast<void*>(sm),
                                   reinterpret_cast<void*>(hk_metrics), &s_orig_metrics);

        auto* eds = GetProcAddress(user32, xorstr_("EnumDisplaySettingsW"));
        if (eds) create_and_enable(reinterpret_cast<void*>(eds),
                                    reinterpret_cast<void*>(hk_enum_display), &s_orig_enum_display);

        auto* edsx = GetProcAddress(user32, xorstr_("EnumDisplaySettingsExW"));
        if (edsx) create_and_enable(reinterpret_cast<void*>(edsx),
                                     reinterpret_cast<void*>(hk_enum_display_ex), &s_orig_enum_display_ex);

        auto* edd = GetProcAddress(user32, xorstr_("EnumDisplayDevicesW"));
        if (edd) create_and_enable(reinterpret_cast<void*>(edd),
                                    reinterpret_cast<void*>(hk_enum_devices), &s_orig_enum_devices);
    }

    auto* gdi32 = GetModuleHandleA(xorstr_("gdi32.dll"));
    if (gdi32) {
        auto* gdc = GetProcAddress(gdi32, xorstr_("GetDeviceCaps"));
        if (gdc) create_and_enable(reinterpret_cast<void*>(gdc),
                                    reinterpret_cast<void*>(hk_devcaps), &s_orig_devcaps);
    }

    HMODULE sc = GetModuleHandleA(xorstr_("steamclient64.dll"));
    if (sc) hook_steamclient();

    HMODULE t0 = GetModuleHandleA(xorstr_("tier0_s64.dll"));
    if (t0) hook_tier0();

    HMODULE ntdll = GetModuleHandleA(xorstr_("ntdll.dll"));
    if (ntdll) {
        auto LdrRegister = reinterpret_cast<FnLdrRegisterDllNotification>(
            GetProcAddress(ntdll, xorstr_("LdrRegisterDllNotification")));
        if (LdrRegister) {
            static void* cookie = nullptr;
            LdrRegister(0, reinterpret_cast<void*>(on_dll_loaded), nullptr, &cookie);
        }
    }
}
