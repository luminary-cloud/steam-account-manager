#pragma once

namespace sam::platform::startup_task {

// Creates/removes a Scheduled Task that launches this exe with "--startup" at
// logon, highest privileges. A task is required because the app is
// requireAdministrator and can't auto-start from the HKCU Run key (Windows skips
// elevated apps there).
bool set_run_at_logon(bool enable);

bool is_run_at_logon_enabled();

}  // namespace sam::platform::startup_task
