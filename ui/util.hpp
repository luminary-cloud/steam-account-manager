#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

#include <imgui.h>

namespace sam::ui {

std::string to_utf8(const std::wstring& w);
std::wstring to_wide(std::string_view u8);

inline std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// CS2 weekly XP / shop reset: Wednesday at this UTC hour.
inline constexpr int kWeeklyResetWeekday = 3;  // tm_wday: 0=Sun .. 3=Wed
inline constexpr int kWeeklyResetHourUtc = 1;  // 01:00 UTC

// Unix time (s, UTC) of the next weekly reset strictly after from_unix. If from_unix
// is already on Wednesday past the reset hour, this rolls to the following Wednesday.
inline std::int64_t next_weekly_reset(std::int64_t from_unix) {
    const auto t = static_cast<std::time_t>(from_unix);
    std::tm tm{};
    gmtime_s(&tm, &t);
    tm.tm_mday += (kWeeklyResetWeekday - tm.tm_wday + 7) % 7;  // this week's Wednesday
    tm.tm_hour  = kWeeklyResetHourUtc;
    tm.tm_min   = 0;
    tm.tm_sec   = 0;
    tm.tm_isdst = 0;                                           // UTC has no DST
    std::int64_t reset = static_cast<std::int64_t>(_mkgmtime(&tm));
    if (reset <= from_unix) reset += 7 * 86400;                // already past today's reset
    return reset;
}

// "YYYY-MM-DD" (UTC), or empty for a non-positive timestamp.
std::string format_date(std::int64_t unix_seconds);

// Coarse "5m ago" / "3d ago", falling back to format_date past 30 days.
// "never" for a zero/unset timestamp.
std::string format_relative(std::int64_t unix_seconds);

// UTF-8-safe clip to max_w with a trailing "...". Returns the input unchanged when
// it already fits, so callers can compare to detect truncation.
std::string truncate_to_width(const std::string& text, float max_w);

void open_url(const std::string& url);

// Opens a folder (or file) in Explorer / the shell default.
void open_folder(const std::filesystem::path& path);

void hover_tooltip(const char* text);

// Like ImGui::SetTooltip but restores inner WindowPadding the (0,0) theme strips.
void set_tooltip(const char* fmt, ...) IM_FMTARGS(1);

// Horizontal inset (px) of the main content column, applied as the left Indent and
// right item-width inset so full-width items stop at the same edge as widgets.
inline constexpr float kContentPaddingX = 24.0F;

// Like ImGui::Separator/SeparatorText but stop kContentPaddingX short of the right
// edge, lining up with widgets that honor PushItemWidth(-kContentPaddingX).
void separator();
void separator_text(const char* label);

bool begin_styled_modal(const char* name, float width = 420.0F);
void end_styled_modal();

// Push inner WindowPadding so popup items aren't clipped against the border (the
// global theme sets WindowPadding to 0). Pair begin_/end_ symmetrically; end_ only
// runs End*/Pop* when begin_ returned true.
bool begin_styled_combo(const char* label, const char* preview_value,
                        ImGuiComboFlags flags = 0);
void end_styled_combo();

// ImGui::Combo with the same inner padding. It only overrides the popup's *horizontal*
// padding itself and takes the vertical one from the theme's WindowPadding, which is 0
// here, leaving the first/last entry flush against the rounded popup border.
bool styled_combo(const char* label, int* current_item,
                  const char* items_separated_by_zeros);

bool begin_styled_popup(const char* str_id, ImGuiWindowFlags flags = 0);
void end_styled_popup();

// width 0 auto-sizes to the label; positive forces that width and centers the label.
void draw_pill(const char* label, const ImVec4& fill, bool on, float width = 0.0F);

// Two-tone stat chip: dim label, bright value, in a rounded rect (e.g. "Steam Lv 9").
// The 3-arg variant colorizes the value (e.g. Prime -> success green when on).
void draw_stat_chip(const char* label, const char* value);
void draw_stat_chip(const char* label, const char* value, const ImVec4& value_color);

// Uniform-styled button used everywhere. Default ImVec2(0,0) auto-sizes to the label.
bool action_button(const char* label, const ImVec2& size = ImVec2(0, 0));

}  // namespace sam::ui
