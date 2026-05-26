#pragma once

struct ImFont;

namespace sam::ui::fonts {

// Loads body + title fonts from the system Fonts directory (Segoe UI).
// Call once after ImGui::CreateContext but before the first frame.
// Falls back to ImGui's built-in font if Segoe is unavailable.
void load(float scale = 1.0F);

ImFont* body();
ImFont* title();
ImFont* badge();   // Large bold for overlay text (e.g. Premier ELO on rank badge).

}  // namespace sam::ui::fonts
