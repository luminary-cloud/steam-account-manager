#pragma once

#include "app/app_state.hpp"

namespace sam::ui::screens {
namespace settings_detail {

// Writes the DPAPI-wrapped master password so the next launch (or headless logon
// run) opens the vault without a prompt. False if unavailable or the write fails.
bool write_master_pw_cache(app::AppState& state);

// Heavier sections, defined in settings_sections.cpp.
void draw_proxy_section(app::AppState& state);
void draw_confirmations_section(app::AppState& state);
void draw_authenticator_section(app::AppState& state);
void draw_cs2_config_section(app::AppState& state);
// settings_cleaner.cpp: the whole Cleaner tab, including its own modals.
void draw_cleaner_section(app::AppState& state);
void draw_gamesense_section(app::AppState& state);
void draw_luminary_section(app::AppState& state);
void draw_storage_section(app::AppState& state);
void draw_vaults_section(app::AppState& state);

}  // namespace settings_detail
}  // namespace sam::ui::screens
