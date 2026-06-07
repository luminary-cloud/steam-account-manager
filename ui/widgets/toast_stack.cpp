#include "ui/widgets/toast_stack.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include <imgui.h>

#include "ui/theme.hpp"

namespace sam::ui::widgets {

namespace {

constexpr int kMaxVisible = 4;
constexpr float kToastWidth  = 320.0F;
constexpr float kToastMargin = 16.0F;
constexpr float kToastPad    = 10.0F;

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

ImU32 border_color(bool warning) {
    const ImVec4 c = warning ? theme::warning() : theme::danger();
    return IM_COL32(static_cast<int>(c.x * 255),
                    static_cast<int>(c.y * 255),
                    static_cast<int>(c.z * 255), 220);
}

}  // namespace

void ToastStack::push(ToastItem item) {
    if (!item.id.empty()) {
        for (auto& existing : items_) {
            if (existing.id == item.id) {
                existing = std::move(item);
                return;
            }
        }
    }
    items_.push_back(std::move(item));
}

void ToastStack::push_summary(std::string message) {
    // Strip any account-scoped pending toasts from this batch and replace
    // with a single summary line.
    items_.erase(std::remove_if(items_.begin(), items_.end(),
        [](const ToastItem& t) { return !t.account_id.empty(); }),
        items_.end());
    ToastItem t;
    t.id = "summary-" + std::to_string(now_unix());
    t.message = std::move(message);
    t.is_warning = false;
    t.expires_at_unix = now_unix() + 8;
    push(std::move(t));
}

void ToastStack::render(const std::function<void(const std::string&)>& on_click) {
    const auto now = now_unix();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
        [now](const ToastItem& t) {
            return t.expires_at_unix > 0 && t.expires_at_unix <= now;
        }), items_.end());
    if (items_.empty()) return;

    const auto vp = ImGui::GetMainViewport();
    const float origin_x = vp->WorkPos.x + vp->WorkSize.x - kToastWidth - kToastMargin;
    float origin_y = vp->WorkPos.y + vp->WorkSize.y - kToastMargin;

    // The close ("x") button shares the message's first line; budget the text
    // column to wrap a few pixels short of it. The font and global style match
    // inside and outside the toast window, so computing this once keeps the
    // height (CalcTextSize below) and the rendered wrap position in agreement.
    // A mismatch makes a long message wrap into an extra line at render time
    // that the fixed-height, no-scrollbar window then clips.
    const float close_w =
        ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0F;
    const float wrap_w = kToastWidth - kToastPad * 2.0F - close_w - 6.0F;

    int drawn = 0;
    for (auto it = items_.rbegin(); it != items_.rend() && drawn < kMaxVisible; ++it, ++drawn) {
        const ImVec2 ts = ImGui::CalcTextSize(it->message.c_str(), nullptr, false, wrap_w);
        const float h = ts.y + kToastPad * 2.0F;
        origin_y -= h;

        ImGui::SetNextWindowPos(ImVec2(origin_x, origin_y));
        ImGui::SetNextWindowSize(ImVec2(kToastWidth, h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kToastPad, kToastPad));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::panel());
        ImGui::PushStyleColor(ImGuiCol_Border, ImColor(border_color(it->is_warning)).Value);

        const std::string win_name = "##toast-" + std::to_string(drawn) + "-" + it->id;
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;
        if (ImGui::Begin(win_name.c_str(), nullptr, flags)) {
            // Wrap at the same width used for the height above. PushTextWrapPos
            // takes a window-local X and the text starts at local x = kToastPad,
            // so kToastPad + wrap_w yields an effective wrap width of wrap_w.
            ImGui::PushTextWrapPos(kToastPad + wrap_w);
            ImGui::TextUnformatted(it->message.c_str());
            ImGui::PopTextWrapPos();

            const ImVec2 win_pos = ImGui::GetWindowPos();
            const ImVec2 win_size = ImGui::GetWindowSize();
            const ImVec2 x_pos(win_pos.x + win_size.x - kToastPad - close_w,
                               win_pos.y + kToastPad);
            ImGui::SetCursorScreenPos(x_pos);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::SmallButton("x")) {
                it->expires_at_unix = now;  // mark for prune next frame
            }
            ImGui::PopStyleColor();

            if (!it->account_id.empty() && ImGui::IsWindowHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const ImVec2 mouse = ImGui::GetMousePos();
                if (mouse.x < x_pos.x - 4.0F) {
                    if (on_click) on_click(it->account_id);
                    it->expires_at_unix = now;
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);

        origin_y -= 8.0F;
    }
}

void ToastStack::clear() { items_.clear(); }

}  // namespace sam::ui::widgets
