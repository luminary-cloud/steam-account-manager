#include "app/app_paths.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"

namespace sam::app {

void ensure_data_dirs() {
    std::error_code ec;
    std::filesystem::create_directories(platform::data_dir(), ec);
    std::filesystem::create_directories(platform::log_dir(), ec);
    std::filesystem::create_directories(platform::vaults_root(), ec);
    std::filesystem::create_directories(platform::cache_dir(), ec);
    std::filesystem::create_directories(platform::resources_dir(), ec);
    std::filesystem::create_directories(platform::tools_dir(), ec);
    if (!platform::active_vault_id().empty()) {
        std::filesystem::create_directories(platform::active_vault_dir(), ec);
    }
}

std::filesystem::path vault_path()    { return platform::vault_path(); }
std::filesystem::path settings_path() { return platform::settings_path(); }

std::filesystem::path vault_settings_path() {
    return platform::active_vault_dir() / "settings.json";
}
std::filesystem::path master_pw_cache_path() {
    return platform::active_vault_dir() / "master_pw.bin";
}
std::filesystem::path notifications_path() {
    return platform::active_vault_dir() / "notifications.json";
}
std::filesystem::path conf_audit_path() {
    return platform::active_vault_dir() / "conf_audit.json";
}
std::filesystem::path trade_audit_path() {
    return platform::active_vault_dir() / "trade_audit.json";
}
std::filesystem::path cs2_video_template_path() {
    return platform::resources_dir() / "cs2_video_template.txt";
}
std::filesystem::path cs2_730_template_dir() {
    return platform::resources_dir() / "cs2_730_template";
}
std::filesystem::path userdata_template_dir() {
    return platform::resources_dir() / "userdata_template";
}
std::filesystem::path browser_login_html_path() {
    return platform::browser_cache_dir() / "login.html";
}
std::filesystem::path browser_profile_dir() {
    return platform::browser_cache_dir() / "profile";
}
std::filesystem::path log_dir()       { return platform::log_dir(); }

bool run_as_admin_hint() {

    std::ifstream in(settings_path());
    if (!in) return true;
    try {
        nlohmann::json j;
        in >> j;
        if (j.is_object() && j.contains("run_as_admin") && j["run_as_admin"].is_boolean()) {
            return j["run_as_admin"].get<bool>();
        }
    } catch (const std::exception&) {

    }
    return true;
}

}  // namespace sam::app
