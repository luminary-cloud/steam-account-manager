#include "platform/dpi.hpp"

#include <windows.h>

namespace sam::platform::dpi {

namespace {

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);

}  // namespace

void enable_per_monitor_v2() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    auto set_ctx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_ctx) {
        if (set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        return;
    }
    // Fallback chain for older Win10 builds.
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto fn = reinterpret_cast<SetProcessDpiAwarenessFn>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (fn) fn(2);  // PROCESS_PER_MONITOR_DPI_AWARE
    }
}

}  // namespace sam::platform::dpi
