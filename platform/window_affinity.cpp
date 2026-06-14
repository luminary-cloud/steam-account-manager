#include "platform/window_affinity.hpp"

#include <windows.h>

#include "core/log.hpp"

namespace sam::platform {

void set_capture_excluded(HWND hwnd, bool excluded) {
    if (!hwnd) return;
    const DWORD affinity = excluded ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (!SetWindowDisplayAffinity(hwnd, affinity)) {
        SAM_LOG_WARN("window_affinity: SetWindowDisplayAffinity({}) failed: {}",
                     excluded ? "exclude" : "none", GetLastError());
    }
}

}  // namespace sam::platform
