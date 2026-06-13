#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "app/app_state.hpp"
#include "core/account_store/account.hpp"
#include "core/account_store/store.hpp"

namespace sam::ui::widgets {

constexpr std::size_t kBundlePassphraseBufLen = 128;

// Owned by the calling screen as a static so it survives across frames while
// the modal is up.
struct ExportBundleState {
    bool open = false;
    // Empty = export every account in the vault.
    std::vector<std::string> account_ids;
    std::array<char, kBundlePassphraseBufLen> passphrase_buf{};
    std::array<char, kBundlePassphraseBufLen> confirm_buf{};
    std::string status;
    bool done = false;
    bool show_passphrase = false;
};

struct ImportBundleState {
    bool open = false;
    std::filesystem::path picked_path;
    std::array<char, kBundlePassphraseBufLen> passphrase_buf{};
    std::string error;
    bool have_preview = false;
    core::Vault preview_vault;
    core::store::MergeReport preview_report;
    bool done = false;
    std::string status;
    bool show_passphrase = false;
};

// Empty `ids` exports every account in the vault.
void request_export_bundle(ExportBundleState& state,
                            std::vector<std::string> ids);

void request_import_bundle(ImportBundleState& state,
                            std::filesystem::path path);

// Safe to call every frame; render nothing until a request comes in.
void draw_export_bundle_modal(app::AppState& app, ExportBundleState& state);
void draw_import_bundle_modal(app::AppState& app, ImportBundleState& state);

}  // namespace sam::ui::widgets
