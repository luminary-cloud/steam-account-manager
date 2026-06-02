#include "ui/util.hpp"

#include <cstdarg>

#include <windows.h>
#include <shellapi.h>

#include "ui/theme.hpp"

namespace sam::ui {

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring to_wide(std::string_view u8) {
    if (u8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()),
                                       nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), out.data(), n);
    return out;
}

void open_url(const std::string& url) {
    const auto w = to_wide(url);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void hover_tooltip(const char* text) {
    if (ImGui::IsItemHovered() && text && *text) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0F);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleVar();
    }
}

void set_tooltip(const char* fmt, ...) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
    ImGui::PopStyleVar();
}

bool begin_styled_modal(const char* name, float width) {
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    bool open = ImGui::BeginPopupModal(
        name, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!open) ImGui::PopStyleVar();
    return open;
}

void end_styled_modal() {
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

bool begin_styled_combo(const char* label, const char* preview_value, ImGuiComboFlags flags) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    const bool open = ImGui::BeginCombo(label, preview_value, flags);
    if (!open) ImGui::PopStyleVar();
    return open;
}

void end_styled_combo() {
    ImGui::EndCombo();
    ImGui::PopStyleVar();
}

bool begin_styled_popup(const char* str_id, ImGuiWindowFlags flags) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    const bool open = ImGui::BeginPopup(str_id, flags);
    if (!open) ImGui::PopStyleVar();
    return open;
}

void end_styled_popup() {
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

bool action_button(const char* label, const ImVec2& size) {
    // Uniform "medium" height across all screens. Slightly tighter horizontal
    // padding than ImGui's default; vertical padding is computed so the final
    // height lands at ~26 px regardless of the surrounding style.
    constexpr float kButtonHeight = 26.0F;
    const float font_h = ImGui::GetFontSize();
    const float pad_y = (kButtonHeight - font_h) * 0.5F;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0F, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0F);
    const bool clicked = ImGui::Button(label, ImVec2(size.x, size.y == 0 ? 0 : size.y));
    ImGui::PopStyleVar(2);
    return clicked;
}

void draw_pill(const char* label, const ImVec4& fill, bool on, float width) {
    const ImVec2 pad{8, 3};
    const ImVec2 sz = ImGui::CalcTextSize(label);
    const float box_w = width > 0.0F ? width : sz.x + pad.x * 2.0F;
    const ImVec2 box{box_w, sz.y + pad.y * 2.0F};
    const ImVec2 cursor = ImGui::GetCursorScreenPos();

    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 bg_col = on
        ? ImGui::ColorConvertFloat4ToU32(fill)
        : IM_COL32(255, 255, 255, 20);
    const ImU32 fg_col = on ? IM_COL32_WHITE : IM_COL32(160, 160, 160, 255);

    dl->AddRectFilled(cursor, ImVec2(cursor.x + box.x, cursor.y + box.y), bg_col, 4.0F);
    // Center the label horizontally within the (possibly stretched) box.
    const float text_x = cursor.x + (box.x - sz.x) * 0.5F;
    dl->AddText(ImVec2(text_x, cursor.y + pad.y), fg_col, label);
    ImGui::Dummy(box);
}

namespace {

void draw_stat_chip_impl(const char* label, const char* value, ImU32 value_col) {
    const ImVec2 pad{8, 3};
    const float  inner_gap = 5.0F;
    const ImVec2 ls = ImGui::CalcTextSize(label);
    const ImVec2 vs = ImGui::CalcTextSize(value);
    const float  h  = (ls.y > vs.y ? ls.y : vs.y);
    const ImVec2 box{ls.x + inner_gap + vs.x + pad.x * 2.0F, h + pad.y * 2.0F};
    const ImVec2 cursor = ImGui::GetCursorScreenPos();

    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cursor, ImVec2(cursor.x + box.x, cursor.y + box.y),
                      IM_COL32(255, 255, 255, 14), 6.0F);

    const ImU32 label_col = ImGui::GetColorU32(theme::dim_text());
    dl->AddText(ImVec2(cursor.x + pad.x, cursor.y + pad.y), label_col, label);
    dl->AddText(ImVec2(cursor.x + pad.x + ls.x + inner_gap, cursor.y + pad.y),
                value_col, value);
    ImGui::Dummy(box);
}

}  // namespace

void draw_stat_chip(const char* label, const char* value) {
    draw_stat_chip_impl(label, value, ImGui::GetColorU32(theme::text()));
}

void draw_stat_chip(const char* label, const char* value, const ImVec4& value_color) {
    draw_stat_chip_impl(label, value, ImGui::ColorConvertFloat4ToU32(value_color));
}

}  // namespace sam::ui
