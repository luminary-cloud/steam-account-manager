#pragma once

namespace sam::platform::startup_task {

// Creates (enable) or removes (disable) a Scheduled Task that launches this
// executable with "--startup" at user logon, with highest privileges. A task
// is required because the app is marked requireAdministrator and so cannot
// auto-start from the HKCU Run key (Windows skips elevated apps there).
// Returns true on success.
bool set_run_at_logon(bool enable);

// True if the logon task currently exists.
bool is_run_at_logon_enabled();

}  // namespace sam::platform::startup_task
