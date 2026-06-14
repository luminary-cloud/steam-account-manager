#pragma once

struct HWND__;
using HWND = HWND__*;

namespace sam::platform {

// Toggles WDA_EXCLUDEFROMCAPTURE on `hwnd`. When excluded, screen-capture software
// (OBS, Discord, Snipping Tool) records a blank where the window is while it stays
// visible on the physical monitor. Requires Windows 10 2004+; on older builds the
// call fails and the window is captured normally. Best-effort: logs on failure.
void set_capture_excluded(HWND hwnd, bool excluded);

}  // namespace sam::platform
