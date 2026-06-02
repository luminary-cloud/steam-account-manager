#pragma once

#include "app/app_state.hpp"

namespace sam::ui::widgets {

// Shared with win_main.cpp's WM_NCHITTEST so the caption strip and button block
// line up. Logical pixels at 100% DPI; wnd_proc scales by GetDpiForWindow / 96.
inline constexpr float kTitleBarHeight  = 34.0F;
inline constexpr float kCaptionBtnWidth = 46.0F;
inline constexpr int   kCaptionBtnCount = 3;   // minimize, maximize, close
inline constexpr float kResizeBorder    = 6.0F;

void draw_title_bar(app::AppState& state);

}  // namespace sam::ui::widgets
