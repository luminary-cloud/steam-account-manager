#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
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

void open_url(const std::string& url);
void hover_tooltip(const char* text);

// Drop-in for ImGui::SetTooltip that restores the inner WindowPadding the
// global (0, 0) theme strips, so tooltip text isn't flush against the border.
void set_tooltip(const char* fmt, ...) IM_FMTARGS(1);

bool begin_styled_modal(const char* name, float width = 420.0F);
void end_styled_modal();

// Combo / popup helpers that push a small inner WindowPadding so the popup
// items aren't flush against the popup border. The global theme sets
// WindowPadding to (0, 0) so plain BeginCombo / BeginPopup popups clip their
// first and last item against the border; these wrappers fix that.
//
// Pair begin_/end_ symmetrically; the wrapper only calls End* + Pop* when
// Begin* returned true (same shape as begin_styled_modal/end_styled_modal).
bool begin_styled_combo(const char* label, const char* preview_value,
                        ImGuiComboFlags flags = 0);
void end_styled_combo();

bool begin_styled_popup(const char* str_id, ImGuiWindowFlags flags = 0);
void end_styled_popup();

// Draws a small rounded label "pill" used for ban indicators and similar
// status chips. When `width` is 0 the pill auto-sizes to its label; when
// positive the pill rect is forced to that width and the label is centered.
void draw_pill(const char* label, const ImVec4& fill, bool on, float width = 0.0F);

// Two-tone stat chip: dim label on the left, bright value on the right,
// inside a soft rounded rect. Used for compact key/value stat display
// (e.g. "Steam Lv 9", "XP 2,557"). The 3-arg variant colorizes the value
// (useful for binary-state stats like Prime → success green when on).
void draw_stat_chip(const char* label, const char* value);
void draw_stat_chip(const char* label, const char* value, const ImVec4& value_color);

// Uniform-styled action button used across all screens and cards. Drop-in
// replacement for ImGui::Button / ImGui::SmallButton so every button looks
// the same regardless of where it lives. Use ImVec2(0, 0) (default) to let
// the button auto-size to its label; pass an explicit width when laying out
// a fixed-width action row.
bool action_button(const char* label, const ImVec2& size = ImVec2(0, 0));

}  // namespace sam::ui
