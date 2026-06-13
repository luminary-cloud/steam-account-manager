#pragma once

#include <string>

namespace sam::platform {

// Opens `url` in the default browser pointed at its own `profile_dir` (Chromium
// --user-data-dir, Firefox -profile), isolated from the normal session. False if
// it fell back to a plain open because the browser couldn't be identified.
bool open_isolated_window(const std::wstring& url, const std::wstring& profile_dir);

}  // namespace sam::platform
