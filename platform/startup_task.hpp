#pragma once

#include <string>

namespace sam::platform::startup_task {

// Creates/removes a single Scheduled Task that launches this exe at logon. A task is used
// rather than the HKCU Run key because Windows skips elevated apps there, and because only
// a task can start the app already elevated (no UAC prompt at logon).
//
// `args` is the command line to pass (L"--startup" for the headless refresh, L"--minimized"
// or L"" for the GUI). `persistent` drops the 5-minute execution limit, needed for the
// long-running GUI. `highest` must track Settings::run_as_admin, or a task left at highest
// would silently elevate at logon after admin was turned off; registering at highest needs
// admin, so this returns false if an unelevated process asks. Re-registers in place, so
// switching modes is another enable call. All three are ignored when disabling.
bool set_run_at_logon(bool enable, const std::wstring& args = L"--startup",
                      bool persistent = false, bool highest = true);

bool is_run_at_logon_enabled();

}  // namespace sam::platform::startup_task
