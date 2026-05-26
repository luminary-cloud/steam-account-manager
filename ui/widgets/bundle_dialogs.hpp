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

// State for the multi-step export modal. Owned by the calling screen as a
// static variable so the state survives across frames while the modal is up.
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

// State for the multi-step import modal.
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

// Resets `state` and asks ImGui to open the export modal on the next frame.
// `ids` is the list of accounts to export; pass an empty vector to export
// every account in the vault.
void request_export_bundle(ExportBundleState& state,
                            std::vector<std::string> ids);

// Resets `state` and asks ImGui to open the import modal on the next frame.
// `path` is the bundle file the user has already picked via a file dialog.
void request_import_bundle(ImportBundleState& state,
                            std::filesystem::path path);

// Renders the export-bundle modal. Safe to call every frame; renders nothing
// when no export has been requested.
void draw_export_bundle_modal(app::AppState& app, ExportBundleState& state);

// Renders the import-bundle modal. Safe to call every frame; renders nothing
// when no import has been requested.
void draw_import_bundle_modal(app::AppState& app, ImportBundleState& state);

}  // namespace sam::ui::widgets
