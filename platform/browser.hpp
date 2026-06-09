#pragma once

#include <string>

namespace sam::platform {

// Opens `url` in the user's default browser using a dedicated profile directory
// (`profile_dir`), isolated from their normal browsing session. Detects the
// default browser and launches it pointed at its own profile (Chromium
// `--user-data-dir`, Firefox `-profile`). Returns true if the isolated profile
// was launched; false if it had to fall back to a plain open because the
// browser couldn't be identified or the launch failed.
bool open_isolated_window(const std::wstring& url, const std::wstring& profile_dir);

}  // namespace sam::platform
