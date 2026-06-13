#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <unordered_set>

#include "app/app_state.hpp"

namespace sam::ui::widgets {

constexpr std::size_t kSdaPassphraseBufLen = 128;

struct BackupDialogState {
    bool open = false;
    std::string account_id;
    // Rebuilt on demand when the stored uri is empty. Not persisted.
    std::string synthesized_uri;
};

struct ExportMafilesState {
    bool open = false;
    std::unordered_set<std::string> selected_ids;
    std::array<char, kSdaPassphraseBufLen> passphrase_buf{};
    std::array<char, kSdaPassphraseBufLen> confirm_buf{};
    bool show_passphrase = false;
    std::string status;
    int written = 0;
    int skipped = 0;
};

struct ShowSecretsState {
    bool open = false;
};

void request_backup(BackupDialogState& state, std::string account_id);
void request_export_mafiles(ExportMafilesState& state);
void request_show_secrets(ShowSecretsState& state);

void draw_backup_modal(app::AppState& app, BackupDialogState& state);
void draw_export_mafiles_modal(app::AppState& app, ExportMafilesState& state);
void draw_show_secrets_modal(app::AppState& app, ShowSecretsState& state);

}  // namespace sam::ui::widgets
