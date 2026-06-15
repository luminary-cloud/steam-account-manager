#pragma once

#include <cstdint>
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

// Result of importing a single NFA refresh token ("SteamID----JWT" or a bare JWT).
struct JwtImportResult {
    bool ok = false;
    bool merged = false;           // existing account updated rather than created
    bool client_audience = false;  // token's aud includes "client" (client-signable)
    bool steam_issuer = true;      // token's iss is Steam (matches Python check_token)
    bool expired = false;
    std::int64_t expires = 0;
    std::uint64_t steam_id = 0;
    std::string error;
    std::string account_id;
    std::string login;
};

// Parses + validates a "SteamID----JWT" (or bare JWT) line, then creates or updates
// the matching NFA account in the vault in place. Caller persists the vault on ok.
JwtImportResult import_jwt_token(app::AppState& state, const std::string& raw);

}  // namespace add_account_detail
}  // namespace sam::ui::screens
