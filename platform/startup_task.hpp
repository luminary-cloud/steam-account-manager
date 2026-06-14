#pragma once

#include <string>

namespace sam::platform::startup_task {

// Creates/removes a single Scheduled Task that launches this exe at logon with
// highest privileges. A task is required because the app is requireAdministrator
// and can't auto-start from the HKCU Run key (Windows skips elevated apps there).
//
// `args` are the command-line arguments to pass (e.g. L"--startup" for the
// headless refresh, L"--minimized" or L"" for the full GUI). `persistent` removes
// the 5-minute execution-time limit, which is needed for the long-running GUI but
// would never trigger for the headless one-shot. Re-registers the same task in
// place, so switching modes is just another enable call. `args`/`persistent` are
// ignored when disabling.
bool set_run_at_logon(bool enable, const std::wstring& args = L"--startup",
                      bool persistent = false);

bool is_run_at_logon_enabled();

}  // namespace sam::platform::startup_task
