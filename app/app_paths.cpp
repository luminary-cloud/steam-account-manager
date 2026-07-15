#include "app/app_paths.hpp"

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
    // Once an active vault is chosen, make sure its folder exists too.
    if (!platform::active_vault_id().empty()) {
        std::filesystem::create_directories(platform::active_vault_dir(), ec);
    }
}

std::filesystem::path vault_path()    { return platform::vault_path(); }
std::filesystem::path settings_path() { return platform::settings_path(); }
// The auto-unlock cache, notifications and audit logs are per-vault: they live
// inside the active vault's folder, alongside vault.bin.
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
std::filesystem::path browser_login_html_path() {
    return platform::browser_cache_dir() / "login.html";
}
std::filesystem::path browser_profile_dir() {
    return platform::browser_cache_dir() / "profile";
}
std::filesystem::path log_dir()       { return platform::log_dir(); }

}  // namespace sam::app
