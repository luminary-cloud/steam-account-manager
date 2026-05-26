#pragma once

namespace sam::platform::dpi {

// Enables per-monitor v2 DPI awareness if the OS supports it.
// Safe to call at process start before creating any windows.
void enable_per_monitor_v2();

}  // namespace sam::platform::dpi
