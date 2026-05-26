#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace sam::ui::widgets {

struct ToastItem {
    std::string id;
    std::string message;
    std::string account_id;     // empty for non-account toasts
    bool is_warning = false;    // false = danger styling
    std::int64_t expires_at_unix = 0;
};

class ToastStack {
public:
    // Append. Caller fills duration; expires_at_unix is detected_unix + duration.
    void push(ToastItem item);

    // Replaces all pending toasts whose account_id matches `account_id` with
    // a single summary entry. Used when a refresh batch produces many events
    // and the per-account stack would be noisy.
    void push_summary(std::string message);

    // UI-thread only. Renders any active toasts in the bottom-right of the
    // main viewport and prunes expired ones. on_click is invoked with the
    // toast's account_id when the user clicks the body of a toast (empty
    // for summary toasts).
    void render(const std::function<void(const std::string&)>& on_click);

    void clear();

private:
    std::deque<ToastItem> items_;
};

}  // namespace sam::ui::widgets
