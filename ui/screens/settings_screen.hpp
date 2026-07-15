#pragma once

#include "app/app_state.hpp"

namespace sam::ui::screens {

// Sub-rail tabs in draw_settings. Values must match the kCategories[] order in
// settings_screen.cpp (guarded by static_assert there). Used to open a specific
// tab via AppState::pending_settings_category.
enum class SettingsCategory {
    General = 0,
    SecurityPrivacy,
    Appearance,
    Notifications,
    SteamGuard,
    LaunchSteam,
    NetworkData,
    Cs2,
    HwidSpoofer,
    Vaults,
};

void draw_settings(app::AppState& state);

}  // namespace sam::ui::screens
