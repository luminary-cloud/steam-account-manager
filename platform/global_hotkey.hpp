#pragma once

#include <cstdint>

struct HWND__;
using HWND = HWND__*;

namespace sam::platform::global_hotkey {

// Single app-wide hotkey id (wParam of WM_HOTKEY).
inline constexpr int kCopyCodeId = 1001;

// `mods` is a MOD_* bitmask, `vk` a virtual-key code. False if the combo is
// already owned by another process.
bool register_hotkey(HWND hwnd, int id, std::uint32_t mods, std::uint32_t vk);

// Idempotent.
void unregister_hotkey(HWND hwnd, int id);

}  // namespace sam::platform::global_hotkey
