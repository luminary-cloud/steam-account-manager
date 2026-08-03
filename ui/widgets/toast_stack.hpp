#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace sam::ui::widgets {

enum class ToastClickAction { None, Account, Settings };

struct ToastItem {
    std::string id;
    std::string message;
    std::string account_id;     // empty for non-account toasts
    bool is_warning = false;    // false = danger styling
    ToastClickAction on_click_action = ToastClickAction::None;
    // Which settings tab a Settings-action toast opens: a screens::SettingsCategory value,
    // as an int so this header doesn't have to pull in the settings screen. Ignored unless
    // on_click_action is Settings.
    int settings_category = 0;
    std::int64_t expires_at_unix = 0;
};

class ToastStack {
public:
    void push(ToastItem item);

    // Replaces all account-scoped pending toasts with one summary entry, so a
    // refresh batch producing many events doesn't flood the stack.
    void push_summary(std::string message);

    // UI-thread only. Prunes expired toasts; on_click fires with the toast when
    // its body is clicked.
    void render(const std::function<void(const ToastItem&)>& on_click);

    void clear();

private:
    std::deque<ToastItem> items_;
};

}  // namespace sam::ui::widgets
