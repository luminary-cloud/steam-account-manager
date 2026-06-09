#pragma once

#include <filesystem>

namespace sam::app {

void ensure_data_dirs();

std::filesystem::path vault_path();
std::filesystem::path settings_path();
std::filesystem::path master_pw_cache_path();
std::filesystem::path notifications_path();
std::filesystem::path conf_audit_path();
std::filesystem::path trade_audit_path();
std::filesystem::path cs2_video_template_path();
std::filesystem::path cs2_730_template_dir();
// Scratch HTML page that signs a browser in to an account (overwritten per use).
std::filesystem::path browser_login_html_path();
// Dedicated browser profile dir for the "open in browser" feature, isolated
// from the user's normal browsing session.
std::filesystem::path browser_profile_dir();
std::filesystem::path log_dir();

}  // namespace sam::app
