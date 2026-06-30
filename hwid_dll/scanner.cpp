#include "scanner.hpp"
#include <windows.h>
#include <vector>

struct ModuleRange {
    uintptr_t base = 0;
    size_t    size = 0;
};

static ModuleRange get_module_range(const char* module_name) {
    ModuleRange r;
    HMODULE mod = GetModuleHandleA(module_name);
    if (!mod) return r;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    r.base = reinterpret_cast<uintptr_t>(mod);
    r.size = nt->OptionalHeader.SizeOfImage;
    return r;
}

uintptr_t scanner::find_pattern(const char* module_name, const char* pattern) {
    auto range = get_module_range(module_name);
    if (!range.base) return 0;

    std::vector<int> bytes;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') {
            bytes.push_back(-1);
            p += (p[1] == '?') ? 2 : 1;
        } else {
            bytes.push_back(static_cast<int>(strtoul(p, nullptr, 16)));
            p += 2;
        }
    }

    auto* scan_start = reinterpret_cast<uint8_t*>(range.base);
    size_t scan_size = range.size - bytes.size();

    for (size_t i = 0; i < scan_size; ++i) {
        bool found = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] != -1 && scan_start[i + j] != static_cast<uint8_t>(bytes[j])) {
                found = false;
                break;
            }
        }
        if (found)
            return range.base + i;
    }
    return 0;
}

uintptr_t scanner::resolve_call(uintptr_t address) {
    if (!address) return 0;
    auto* p = reinterpret_cast<uint8_t*>(address);
    if (*p != 0xE8) return 0;
    int32_t rel = *reinterpret_cast<int32_t*>(p + 1);
    return address + 5 + rel;
}

void* scanner::vtable_swap(void* vtable_base, int index, void* new_func) {
    auto** vtable = static_cast<void**>(vtable_base);
    void* original = vtable[index];

    DWORD old_protect;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect);
    vtable[index] = new_func;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);

    return original;
}
