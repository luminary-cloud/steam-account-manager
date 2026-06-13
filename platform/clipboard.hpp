#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace sam::platform::clipboard {

// `text` is UTF-8. Returns false on failure.
bool set_text(std::string_view text);

// Clears the clipboard after `delay`, skipping the clear if the user copied
// something else meanwhile (sequence number checked before clearing).
void set_text_with_auto_clear(std::string_view text,
                               std::chrono::seconds delay);

}  // namespace sam::platform::clipboard
