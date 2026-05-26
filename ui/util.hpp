#pragma once

#include <chrono>
#include <cstdint>
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

void open_url(const std::string& url);
void hover_tooltip(const char* text);

bool begin_styled_modal(const char* name, float width = 420.0F);
void end_styled_modal();

// Draws a small rounded label "pill" used for ban indicators and similar
// status chips. When `width` is 0 the pill auto-sizes to its label; when
// positive the pill rect is forced to that width and the label is centered.
void draw_pill(const char* label, const ImVec4& fill, bool on, float width = 0.0F);

// Uniform-styled action button used across all screens and cards. Drop-in
// replacement for ImGui::Button / ImGui::SmallButton so every button looks
// the same regardless of where it lives. Use ImVec2(0, 0) (default) to let
// the button auto-size to its label; pass an explicit width when laying out
// a fixed-width action row.
bool action_button(const char* label, const ImVec2& size = ImVec2(0, 0));

}  // namespace sam::ui
