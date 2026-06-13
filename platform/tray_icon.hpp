#pragma once

#include <string>

struct HWND__;
using HWND = HWND__*;

namespace sam::platform::tray_icon {

// Posted to the owner window; decode the event from LOWORD(lParam)
// (NOTIFYICON_VERSION_4 semantics).
inline constexpr unsigned kCallbackMessage = 0x8000 + 2;  // WM_APP + 2
inline constexpr unsigned kIconId = 1;

// Records owner/icon but doesn't add the icon yet; it appears on first show_balloon.
void set_owner(HWND owner, unsigned icon_resource_id);

// Idempotent.
void remove();

// UTF-8 title/message; adds the icon if needed. `warning` picks the warning glyph.
bool show_balloon(const std::string& title, const std::string& message, bool warning);

bool owner_is_foreground();

}  // namespace sam::platform::tray_icon
