#include "platform/global_hotkey.hpp"

#include <windows.h>

namespace sam::platform::global_hotkey {

bool register_hotkey(HWND hwnd, int id, std::uint32_t mods, std::uint32_t vk) {
    if (vk == 0) return false;
    return ::RegisterHotKey(hwnd, id, mods | MOD_NOREPEAT, vk) != 0;
}

void unregister_hotkey(HWND hwnd, int id) {
    ::UnregisterHotKey(hwnd, id);
}

}  // namespace sam::platform::global_hotkey
