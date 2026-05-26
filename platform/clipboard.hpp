#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace sam::platform::clipboard {

// Sets the clipboard to `text` (UTF-8). Returns false on failure.
bool set_text(std::string_view text);

// Sets the clipboard to `text` and schedules a worker that clears the clipboard
// after `delay` seconds. If the user copies something else in the meantime the
// clear is skipped (the clipboard sequence number is checked before clearing).
void set_text_with_auto_clear(std::string_view text,
                               std::chrono::seconds delay);

}  // namespace sam::platform::clipboard
