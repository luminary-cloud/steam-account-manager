#include "core/launch/code_clipboard.hpp"

#include <chrono>

#include "core/log.hpp"
#include "core/sda/totp.hpp"
#include "core/time_aligner.hpp"
#include "platform/clipboard.hpp"

namespace sam::launch::code_clipboard {

void copy_code_to_clipboard(std::string_view shared_secret) {
    if (shared_secret.empty()) return;
    const std::string code = sam::sda::generate_code(
        shared_secret, sam::time_aligner::aligned_now());
    if (code.empty()) {
        SAM_LOG_WARN("code_clipboard: TOTP generation failed");
        return;
    }
    sam::platform::clipboard::set_text_with_auto_clear(
        code, std::chrono::seconds(15));
    SAM_LOG_INFO("code_clipboard: 2FA code copied to clipboard (auto-clear 15s)");
}

}  // namespace sam::launch::code_clipboard
