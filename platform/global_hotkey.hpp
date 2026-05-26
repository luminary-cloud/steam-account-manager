#pragma once

#include <cstdint>

struct HWND__;
using HWND = HWND__*;

namespace sam::platform::global_hotkey {

// Single app-wide hotkey id (wParam of WM_HOTKEY).
inline constexpr int kCopyCodeId = 1001;

// Win32 RegisterHotKey wrapper. `mods` is a bitmask of MOD_CONTROL/MOD_ALT/etc;
// `vk` is the virtual-key code. Returns true on success. On failure (combo is
// already owned by another process), returns false and the caller should
// surface a setting-level error.
bool register_hotkey(HWND hwnd, int id, std::uint32_t mods, std::uint32_t vk);

// Idempotent.
void unregister_hotkey(HWND hwnd, int id);

}  // namespace sam::platform::global_hotkey
