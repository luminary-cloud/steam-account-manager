#pragma once

#include <string>

struct HWND__;
using HWND = HWND__*;

namespace sam::platform::tray_icon {

// Message the notification-area icon posts to its owner window. Decode the
// event from LOWORD(lParam) (NOTIFYICON_VERSION_4 semantics).
inline constexpr unsigned kCallbackMessage = 0x8000 + 2;  // WM_APP + 2
inline constexpr unsigned kIconId = 1;

// Records the owner window and icon resource to use. Does not add the icon yet;
// the icon appears lazily on the first show_balloon call.
void set_owner(HWND owner, unsigned icon_resource_id);

// Removes the icon from the notification area. Idempotent.
void remove();

// Shows a balloon notification (UTF-8 title/message), adding the icon first if
// needed. `warning` picks the warning glyph over the info glyph. Returns true
// if the shell accepted it.
bool show_balloon(const std::string& title, const std::string& message, bool warning);

// True if the owner window is currently the foreground window.
bool owner_is_foreground();

}  // namespace sam::platform::tray_icon
