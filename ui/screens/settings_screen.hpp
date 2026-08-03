#pragma once

#include "app/app_state.hpp"

namespace sam::ui::screens {

// Sub-rail tabs in draw_settings. Values must match the kCategories[] order in
// settings_screen.cpp (guarded by static_assert there). Used to open a specific
// tab via AppState::pending_settings_category.
enum class SettingsCategory {
    General = 0,
    Appearance,
    Notifications,
    SteamGuard,
    LaunchSteam,
    Cs2,
    NetworkData,
    Vaults,
    SecurityPrivacy,
    // Safe mode hides these two, so they sit last.
    Cleaner,
    HwidSpoofer,
};

void draw_settings(app::AppState& state);

}  // namespace sam::ui::screens
