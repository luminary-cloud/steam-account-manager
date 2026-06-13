#pragma once

#include <string_view>

namespace sam::launch::code_clipboard {

// Fallback for when the UIA-driven login flow can't reach the Steam Guard digit
// pad. Generates the current TOTP from `shared_secret` and puts it on the clipboard
// with auto-clear so the user can paste it manually. No-op when secret is empty.
void copy_code_to_clipboard(std::string_view shared_secret);

}  // namespace sam::launch::code_clipboard
