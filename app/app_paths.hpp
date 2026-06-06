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
std::filesystem::path log_dir();

}  // namespace sam::app
