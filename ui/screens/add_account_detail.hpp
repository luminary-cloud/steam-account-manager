#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"

namespace sam::ui::screens {
namespace add_account_detail {

std::string generate_ulid();
std::string trim_path(std::string s);
std::vector<std::filesystem::path> scan_dir_for_extension(
    const std::filesystem::path& dir, std::string_view ext_lower);

void draw_import_mafile(app::AppState& state);
void draw_import_info_dat(app::AppState& state);
void draw_import_jwt_token(app::AppState& state);
void draw_full_login(app::AppState& state);

}  // namespace add_account_detail
}  // namespace sam::ui::screens
