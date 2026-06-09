#include "app/app_paths.hpp"

#include "platform/paths.hpp"

namespace sam::app {

void ensure_data_dirs() {
    std::error_code ec;
    std::filesystem::create_directories(platform::data_dir(), ec);
    std::filesystem::create_directories(platform::log_dir(), ec);
}

std::filesystem::path vault_path()    { return platform::vault_path(); }
std::filesystem::path settings_path() { return platform::settings_path(); }
std::filesystem::path master_pw_cache_path() {
    return platform::data_dir() / "master_pw.bin";
}
std::filesystem::path notifications_path() {
    return platform::data_dir() / "notifications.json";
}
std::filesystem::path conf_audit_path() {
    return platform::data_dir() / "conf_audit.json";
}
std::filesystem::path trade_audit_path() {
    return platform::data_dir() / "trade_audit.json";
}
std::filesystem::path cs2_video_template_path() {
    return platform::data_dir() / "cs2_video_template.txt";
}
std::filesystem::path cs2_730_template_dir() {
    return platform::data_dir() / "cs2_730_template";
}
std::filesystem::path browser_login_html_path() {
    return platform::data_dir() / "browser-login.html";
}
std::filesystem::path browser_profile_dir() {
    return platform::data_dir() / "browser-profile";
}
std::filesystem::path log_dir()       { return platform::log_dir(); }

}  // namespace sam::app
