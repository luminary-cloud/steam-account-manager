#include "ui/screens/add_account_screen.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <imgui.h>

#include "app/job_pump.hpp"
#include "core/account_store/store.hpp"
#include "core/crypto/rng.hpp"
#include "core/log.hpp"
#include "core/sda/mafile.hpp"
#include "core/sda/info_dat.hpp"
#include "core/sda/totp.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/steam_login/mobile_auth.hpp"
#include "core/steam_login/session.hpp"
#include "core/time_aligner.hpp"
#include "platform/file_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/bundle_dialogs.hpp"
#include "ui/widgets/master_pw_field.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/stepper.hpp"

namespace sam::ui::screens {

namespace {

std::string generate_ulid() {
    // Not a real ULID, but stable and unique enough for in-app referencing.
    static std::atomic<std::uint64_t> counter{0};
    const auto n = counter.fetch_add(1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx%08llx",
                  static_cast<unsigned long long>(now_seconds()),
                  static_cast<unsigned long long>(n));
    return buf;
}

void draw_manual(app::AppState& state, core::Account* editing) {
    static std::array<char, 64> login{};
    static std::string password;
    static std::string shared_secret;
    static std::string notes;
    static std::string loaded_id;

    constexpr const char* kPwLabel = "Password";
    constexpr const char* kSsLabel = "Shared secret (optional)##sda";

    if (editing && loaded_id != editing->id) {
        std::snprintf(login.data(), login.size(), "%s", editing->login.c_str());
        password = std::string(editing->password.begin(), editing->password.end());
        shared_secret = editing->sda.has_value() ? editing->sda->shared_secret : std::string{};
        notes = to_utf8(editing->notes);
        loaded_id = editing->id;
        widgets::reset_password_visibility(kPwLabel);
        widgets::reset_password_visibility(kSsLabel);
    } else if (!editing && !loaded_id.empty()) {
        login = {};
        password.clear();
        shared_secret.clear();
        notes.clear();
        loaded_id.clear();
        widgets::reset_password_visibility(kPwLabel);
        widgets::reset_password_visibility(kSsLabel);
    }

    ImGui::SeparatorText("Account credentials");

    const bool editing_redacted =
        editing && state.settings.privacy_mode &&
        state.revealed_logins.find(editing->id) == state.revealed_logins.end();
    if (editing_redacted) {
        // Privacy mode + not revealed: stand in for the disabled InputText
        // with a clickable redacted label. The buffer above still holds the
        // real login so save_changes works unchanged.
        widgets::draw_login_text(state, *editing);
        ImGui::SameLine();
        ImGui::TextUnformatted("Login");
    } else {
        ImGui::SetNextItemWidth(280.0F);
        if (editing) ImGui::BeginDisabled();
        ImGui::InputText("Login", login.data(), login.size());
        if (editing) {
            ImGui::EndDisabled();
            hover_tooltip("Login is read-only when editing an existing account.");
        } else {
            hover_tooltip("Steam account name (lowercase username, not the persona/display name).");
        }
    }

    widgets::draw_password_field(kPwLabel, password, false, 280.0F);
    hover_tooltip("Stored encrypted with the vault master password. Used by the Launch button "
                  "to fill the Steam client prompt; never sent to a third party.");

    widgets::draw_password_field(kSsLabel, shared_secret, false, 280.0F);
    hover_tooltip("Base64 shared_secret from your maFile. Optional. When set, the Code button "
                  "generates Steam Guard codes and silent re-login can refresh expired sessions "
                  "without prompting. Leave blank if you don't have it.");
    {
        std::array<char, 1024> nbuf{};
        std::snprintf(nbuf.data(), nbuf.size(), "%s", notes.c_str());
        if (ImGui::InputTextMultiline("Notes", nbuf.data(), nbuf.size(),
                                       ImVec2(380.0F, 90.0F))) {
            notes = nbuf.data();
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(login[0] == 0);
    if (action_button(editing ? "Save changes" : "Save account")) {
        std::string new_id_for_refresh;
        if (editing) {
            editing->password = crypto::make_secure(password);
            editing->notes = to_wide(notes);
            if (!shared_secret.empty()) {
                if (!editing->sda.has_value()) {
                    core::SteamGuardAccount g;
                    g.account_name = editing->login;
                    editing->sda = std::move(g);
                }
                editing->sda->shared_secret = shared_secret;
                if (editing->sda->account_name.empty())
                    editing->sda->account_name = editing->login;
            }
            // If we still don't have a steam_id, opportunistically look one up.
            if (editing->steam_id_64 == 0 && !editing->login.empty()) {
                editing->steam_id_64 = steam_local::lookup_steam_id(editing->login);
                if (editing->steam_id_64 != 0) new_id_for_refresh = editing->id;
            }
        } else {
            core::Account a;
            a.id = generate_ulid();
            a.login = login.data();
            a.password = crypto::make_secure(password);
            a.notes = to_wide(notes);
            a.created_unix = now_seconds();
            if (!shared_secret.empty()) {
                core::SteamGuardAccount g;
                g.account_name = a.login;
                g.shared_secret = shared_secret;
                a.sda = std::move(g);
            }
            a.steam_id_64 = steam_local::lookup_steam_id(a.login);
            if (a.steam_id_64 != 0) new_id_for_refresh = a.id;
            state.vault.accounts.push_back(std::move(a));
        }
        state.vault_dirty = true;
        state.save_vault_if_dirty();
        if (!new_id_for_refresh.empty()) {
            state.refresh_single_account(new_id_for_refresh);
        }
        login = {};
        password.clear();
        shared_secret.clear();
        notes.clear();
        loaded_id.clear();
        state.selected_account_id.clear();
        state.current_screen = app::Screen::Accounts;
    }
    ImGui::EndDisabled();
}

// Strip surrounding double-quotes and whitespace from a path the user typed
// or pasted in (Explorer's "copy as path" wraps in quotes).
std::string trim_path(std::string s) {
    while (!s.empty() && (s.front() == '"' || s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == '"' || s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

// Non-recursive directory scan for files whose name ends (case-insensitively)
// in `ext` (must include leading dot, lowercase).
std::vector<std::filesystem::path> scan_dir_for_extension(
    const std::filesystem::path& dir, std::string_view ext_lower) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return out;
    for (const auto& e : it) {
        if (!e.is_regular_file(ec)) continue;
        std::string name = e.path().filename().string();
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower.size() >= ext_lower.size() &&
            lower.compare(lower.size() - ext_lower.size(),
                          ext_lower.size(), ext_lower) == 0) {
            out.push_back(e.path());
        }
    }
    return out;
}

// maFile single-file import.

struct MafileImportResult {
    bool ok = false;
    bool merged = false;          // true if an existing account was updated
    std::string error;            // populated when !ok
    std::string account_id;       // set when ok
    std::string login;            // for the summary
};

MafileImportResult import_one_mafile(app::AppState& state,
                                      const std::filesystem::path& path,
                                      const std::string& mafile_password,
                                      const std::string& steam_password) {
    MafileImportResult r;
    try {
        auto loaded = sam::sda::load_mafile(path,
                                             crypto::make_secure(mafile_password));

        std::uint64_t resolved_sid = loaded.session_steam_id;
        if (resolved_sid == 0 && !loaded.guard.account_name.empty()) {
            resolved_sid = steam_local::lookup_steam_id(loaded.guard.account_name);
        }
        const bool has_tokens = !loaded.session_access_token.empty() &&
                                !loaded.session_refresh_token.empty();
        auto access  = crypto::make_secure(loaded.session_access_token);
        auto refresh = crypto::make_secure(loaded.session_refresh_token);
        // The maFile may carry session tokens without the steam_id (e.g. some
        // SDA exports omit Session.SteamID). The access_token's `sub` claim
        // is the steam_id_64, so recover it from there before giving up.
        if (resolved_sid == 0 && has_tokens) {
            resolved_sid = steam_login::jwt_steam_id(access);
        }

        core::Account* existing = core::store::find_existing_account(
            state.vault, resolved_sid, loaded.guard.account_name);

        if (existing) {
            existing->sda = std::move(loaded.guard);
            if (existing->steam_id_64 == 0 && resolved_sid != 0)
                existing->steam_id_64 = resolved_sid;
            if (has_tokens && resolved_sid != 0) {
                existing->access_token  = access;
                existing->refresh_token = refresh;
                existing->access_token_expires = steam_login::jwt_expiry(access);
                existing->steam_login_secure = crypto::make_secure(
                    steam_login::make_steam_login_secure(resolved_sid, access));
                if (existing->session_id.empty())
                    existing->session_id = crypto::random_session_id();
            }
            if (!steam_password.empty())
                existing->password = crypto::make_secure(steam_password);
            r.account_id = existing->id;
            r.login = existing->login;
            r.merged = true;
        } else {
            core::Account a;
            a.id = generate_ulid();
            a.login = loaded.guard.account_name;
            a.sda = std::move(loaded.guard);
            a.steam_id_64 = resolved_sid;
            if (has_tokens && resolved_sid != 0) {
                a.access_token  = access;
                a.refresh_token = refresh;
                a.access_token_expires = steam_login::jwt_expiry(access);
                a.steam_login_secure = crypto::make_secure(
                    steam_login::make_steam_login_secure(resolved_sid, access));
                a.session_id = crypto::random_session_id();
            }
            if (!steam_password.empty())
                a.password = crypto::make_secure(steam_password);
            a.created_unix = now_seconds();
            r.account_id = a.id;
            r.login = a.login;
            state.vault.accounts.push_back(std::move(a));
        }
        r.ok = true;
    } catch (const sam::sda::MafileEncrypted&) {
        r.error = "encrypted (need maFile password)";
    } catch (const sam::sda::MafileWrongPassword&) {
        r.error = "wrong maFile password";
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    return r;
}

// info.dat single-file import.

struct InfoDatImportResult {
    bool ok = false;
    int  created = 0;
    int  merged = 0;
    int  accounts_in_file = 0;
    std::string error;            // populated when !ok
    std::string file_label;       // path stem for summary line
    std::vector<std::string> account_ids;   // for follow-up refresh
};

InfoDatImportResult import_one_info_dat(app::AppState& state,
                                         const std::filesystem::path& path,
                                         const std::string& password,
                                         std::string_view field_key) {
    InfoDatImportResult r;
    r.file_label = path.filename().string();
    try {
        auto entries = sam::sda::load_info_dat(path,
                                                crypto::make_secure(password),
                                                field_key);
        r.accounts_in_file = static_cast<int>(entries.size());

        for (auto& e : entries) {
            if (e.name.empty()) continue;

            std::uint64_t sid64 = 0;
            try { sid64 = e.steam_id_str.empty() ? 0 : std::stoull(e.steam_id_str); }
            catch (...) { sid64 = 0; }
            if (sid64 == 0) sid64 = steam_local::lookup_steam_id(e.name);

            core::Account* existing = core::store::find_existing_account(
                state.vault, sid64, e.name);

            auto apply_fields = [&](core::Account& a, bool fresh) {
                a.login = e.name;
                if (!e.password.empty())
                    a.password = crypto::make_secure(e.password);
                if (sid64 != 0) a.steam_id_64 = sid64;

                if (!e.shared_secret.empty()) {
                    if (!a.sda.has_value()) {
                        core::SteamGuardAccount g;
                        g.account_name = e.name;
                        a.sda = std::move(g);
                    }
                    a.sda->shared_secret = e.shared_secret;
                    if (a.sda->account_name.empty())
                        a.sda->account_name = e.name;
                }

                if (!e.description.empty())
                    a.notes = to_wide(e.description);
                if (!e.profile_url.empty())
                    a.web.profile_url = e.profile_url;
                if (!e.avatar_url.empty())
                    a.web.avatar_url_full = e.avatar_url;

                // Ban snapshot from the imported file. Will be overwritten by
                // the first online refresh, but useful immediately so VAC/etc
                // pills show up before that completes.
                a.bans.community_banned = e.community_banned;
                a.bans.vac_banned       = e.vac_banned;
                a.bans.vac_ban_count    = e.vac_ban_count;
                a.bans.game_ban_count   = e.game_ban_count;
                a.bans.days_since_last_ban = e.days_since_last_ban;
                if (!e.economy_ban.empty()) a.bans.economy_ban = e.economy_ban;

                if (fresh) a.created_unix = now_seconds();
            };

            if (existing) {
                apply_fields(*existing, false);
                r.account_ids.push_back(existing->id);
                ++r.merged;
            } else {
                core::Account a;
                a.id = generate_ulid();
                apply_fields(a, true);
                r.account_ids.push_back(a.id);
                state.vault.accounts.push_back(std::move(a));
                ++r.created;
            }
        }
        r.ok = true;
    } catch (const sam::sda::InfoDatEncrypted&) {
        r.error = "file is encrypted: enter the decryption password above";
    } catch (const sam::sda::InfoDatWrongPassword&) {
        r.error = "wrong decryption password";
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    return r;
}

// Import maFile tab.

struct MafileBatchSummary {
    int imported = 0;
    int merged = 0;
    int blank_steam_id = 0;              // imported but no steam_id_64 resolved
    bool web_api_key_missing = false;    // captured at import time for the summary
    std::vector<std::string> failures;   // "<filename>: <error>"
};

void draw_import_mafile(app::AppState& state) {
    static std::array<char, 1024> path_buf{};
    static std::string mafile_password;
    static std::string steam_password;
    static std::vector<std::string> queued;       // from drag-drop
    static MafileBatchSummary summary;
    static bool has_summary = false;
    static std::string error;

    // Drain any drop queue from WM_DROPFILES.
    {
        std::lock_guard lk(state.drop_mutex);
        if (!state.pending_mafile_drops.empty()) {
            queued.insert(queued.end(),
                          std::make_move_iterator(state.pending_mafile_drops.begin()),
                          std::make_move_iterator(state.pending_mafile_drops.end()));
            state.pending_mafile_drops.clear();
            state.pending_mafile_focus = false;
            error.clear();
        }
    }

    ImGui::SeparatorText("Steam Mobile Authenticator file");
    ImGui::TextDisabled("Type a single .maFile path, type a folder to bulk-import every "
                        ".maFile in it, or drag files/folders onto this window.");
    ImGui::SetNextItemWidth(420.0F);
    ImGui::InputText("##path", path_buf.data(), path_buf.size());
    hover_tooltip("Path to a .maFile (single account) or a directory containing one or "
                  "more .maFile files (bulk import).");
    ImGui::SameLine();
    if (action_button("Browse...##mafile-browse")) {
        platform::file_dialog::Options opts;
        opts.parent = state.main_hwnd;
        opts.title = L"Choose maFile";
        opts.allow_multiselect = true;
        opts.filters = {
            {L"Steam mobile authenticator (*.maFile)", L"*.maFile"},
            {L"All files (*.*)", L"*.*"},
        };
        const auto picked = platform::file_dialog::open_file(opts);
        if (picked.ok) {
            if (picked.paths.size() <= 1) {
                std::snprintf(path_buf.data(), path_buf.size(), "%s",
                              picked.path.string().c_str());
            } else {
                for (const auto& p : picked.paths) queued.push_back(p.string());
            }
            error.clear();
        }
    }
    hover_tooltip("Pick a single .maFile via the system dialog. To bulk-import a folder, "
                  "type the folder path above or drop it on the window.");

    if (!queued.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::Text("%zu file(s) queued.", queued.size());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) queued.clear();
    }

    widgets::draw_password_field("maFile password (if encrypted)",
                                  mafile_password, false, 280.0F);
    hover_tooltip("Only required if the maFile was exported with encryption enabled. "
                  "Applied to every file in this batch.");

    widgets::draw_password_field("Steam password (optional)##mafile-steampw",
                                  steam_password, false, 280.0F);
    hover_tooltip("Optional Steam password applied to every account imported in this batch. "
                  "Leave empty if you'll add per-account passwords later.");

    if (state.settings.web_api_key.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("No Steam Web API key set. Stats (level, games, bans, CS2 rank) "
                           "won't populate until you add one in Settings.");
        ImGui::PopStyleColor();
    }

    if (action_button("Import")) {
        error.clear();
        summary = {};
        has_summary = false;

        // Build the work list: typed path (file or directory) + drag-drop queue.
        std::vector<std::filesystem::path> batch;
        std::string typed = trim_path(path_buf.data());
        if (!typed.empty()) {
            std::filesystem::path p(typed);
            std::error_code ec;
            if (std::filesystem::is_directory(p, ec)) {
                auto found = scan_dir_for_extension(p, ".mafile");
                batch.insert(batch.end(), found.begin(), found.end());
            } else {
                batch.emplace_back(std::move(p));
            }
        }
        for (auto& s : queued) batch.emplace_back(s);

        if (batch.empty()) {
            error = "No maFiles to import (type a path or drop files onto the window).";
        } else {
            summary.web_api_key_missing = state.settings.web_api_key.empty();
            std::vector<std::string> account_ids;
            for (const auto& p : batch) {
                auto r = import_one_mafile(state, p, mafile_password, steam_password);
                if (r.ok) {
                    if (r.merged) ++summary.merged; else ++summary.imported;
                    // Accounts with no resolved steam_id_64 won't auto-refresh
                    // and will show up as blank cards until the user provides
                    // one (e.g. by logging into Steam locally, then refreshing).
                    if (auto* a = state.find_account(r.account_id);
                        a && a->steam_id_64 == 0) {
                        ++summary.blank_steam_id;
                    }
                    account_ids.push_back(r.account_id);
                } else {
                    summary.failures.push_back(p.filename().string() + ": " + r.error);
                }
            }
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            // Single-account add: refresh immediately so the user sees their
            // card populated. Bulk add: stagger so 100+ maFiles don't fan out
            // 16+ concurrent Web API requests as the 4-worker pool drains.
            if (account_ids.size() == 1) {
                state.refresh_single_account(account_ids.front());
            } else if (!account_ids.empty()) {
                state.refresh_accounts_staggered(std::move(account_ids));
            }
            has_summary = true;

            // Reset inputs after a successful (non-empty) run.
            path_buf = {};
            mafile_password.clear();
            steam_password.clear();
            queued.clear();
        }
    }

    if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
    }
    if (has_summary) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::Text("Imported %d new, updated %d existing.",
                    summary.imported, summary.merged);
        ImGui::PopStyleColor();
        if (summary.web_api_key_missing) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("No Web API key set: imported accounts will stay "
                               "blank until you add one in Settings and refresh.");
            ImGui::PopStyleColor();
        } else if (summary.blank_steam_id > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("%d account(s) couldn't fetch initial data "
                               "(no Steam ID resolved). Sign into Steam locally "
                               "or use Full login to populate them.",
                               summary.blank_steam_id);
            ImGui::PopStyleColor();
        }
        if (!summary.failures.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::Text("%zu failed:", summary.failures.size());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            for (const auto& f : summary.failures) {
                ImGui::BulletText("%s", f.c_str());
            }
            ImGui::PopStyleColor();
        }
    }
}

// info.dat import tab.

struct InfoDatBatchSummary {
    int files_ok = 0;
    int created  = 0;
    int merged   = 0;
    int accounts_total = 0;
    int blank_steam_id = 0;             // imported but no steam_id_64 resolved
    bool web_api_key_missing = false;
    std::vector<std::string> failures;  // "<filename>: <error>"
};

void draw_import_info_dat(app::AppState& state) {
    static std::array<char, 1024> path_buf{};
    static std::string file_password;
    static std::string field_key;
    static std::vector<std::string> queued;
    static InfoDatBatchSummary summary;
    static bool has_summary = false;
    static std::string error;

    {
        std::lock_guard lk(state.drop_mutex);
        if (!state.pending_info_dat_drops.empty()) {
            queued.insert(queued.end(),
                          std::make_move_iterator(state.pending_info_dat_drops.begin()),
                          std::make_move_iterator(state.pending_info_dat_drops.end()));
            state.pending_info_dat_drops.clear();
            state.pending_info_dat_focus = false;
            error.clear();
        }
    }

    ImGui::SeparatorText("info.dat import");
    ImGui::TextDisabled("Type an info.dat path, type a folder to import every .dat in it, "
                        "or drag the file(s) onto this window.");
    ImGui::SetNextItemWidth(420.0F);
    ImGui::InputText("##info-dat-path", path_buf.data(), path_buf.size());
    hover_tooltip("An info.dat file (XML inside a Rijndael-256/PBKDF2-SHA1 envelope), or a "
                  "directory containing one or more .dat files. Each file can hold many "
                  "accounts.");
    ImGui::SameLine();
    if (action_button("Browse...##info-dat-browse")) {
        platform::file_dialog::Options opts;
        opts.parent = state.main_hwnd;
        opts.title = L"Choose info.dat";
        opts.allow_multiselect = true;
        opts.filters = {
            {L"info.dat (*.dat)", L"*.dat"},
            {L"All files (*.*)", L"*.*"},
        };
        const auto picked = platform::file_dialog::open_file(opts);
        if (picked.ok) {
            if (picked.paths.size() <= 1) {
                std::snprintf(path_buf.data(), path_buf.size(), "%s",
                              picked.path.string().c_str());
            } else {
                for (const auto& p : picked.paths) queued.push_back(p.string());
            }
            error.clear();
        }
    }
    hover_tooltip("Pick a single info.dat via the system dialog. To bulk-import a folder of "
                  ".dat files, type the folder path above or drop it on the window.");

    if (!queued.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::Text("%zu file(s) queued.", queued.size());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##info-dat-clear")) queued.clear();
    }

    widgets::draw_password_field("Decryption password (if encrypted)##info-dat-pw",
                                  file_password, false, 280.0F);
    hover_tooltip("The password the info.dat was encrypted with. Leave empty if the file "
                  "is plain XML.");

    widgets::draw_password_field("Field key (optional)##info-dat-key",
                                  field_key, false, 280.0F);
    hover_tooltip("The built-in key list auto-detects most exports. Set this only if your "
                  "info.dat was produced by a build whose field key isn't in the list.");

    if (state.settings.web_api_key.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("No Steam Web API key set. Stats (level, games, bans, CS2 rank) "
                           "won't populate until you add one in Settings.");
        ImGui::PopStyleColor();
    }

    if (action_button("Import##info-dat-import")) {
        error.clear();
        summary = {};
        has_summary = false;

        std::vector<std::filesystem::path> batch;
        std::string typed = trim_path(path_buf.data());
        if (!typed.empty()) {
            std::filesystem::path p(typed);
            std::error_code ec;
            if (std::filesystem::is_directory(p, ec)) {
                auto found = scan_dir_for_extension(p, ".dat");
                batch.insert(batch.end(), found.begin(), found.end());
            } else {
                batch.emplace_back(std::move(p));
            }
        }
        for (auto& s : queued) batch.emplace_back(s);

        if (batch.empty()) {
            error = "No info.dat files to import.";
        } else {
            summary.web_api_key_missing = state.settings.web_api_key.empty();
            std::vector<std::string> all_ids;
            for (const auto& p : batch) {
                auto r = import_one_info_dat(state, p, file_password, field_key);
                if (r.ok) {
                    ++summary.files_ok;
                    summary.created += r.created;
                    summary.merged  += r.merged;
                    summary.accounts_total += r.accounts_in_file;
                    for (const auto& aid : r.account_ids) {
                        if (auto* a = state.find_account(aid);
                            a && a->steam_id_64 == 0) {
                            ++summary.blank_steam_id;
                        }
                    }
                    all_ids.insert(all_ids.end(),
                                    r.account_ids.begin(), r.account_ids.end());
                } else {
                    summary.failures.push_back(r.file_label + ": " + r.error);
                }
            }
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            // info.dat files can carry many accounts each; stagger to avoid a
            // burst on the Web API. Single-account result still refreshes
            // immediately.
            if (all_ids.size() == 1) {
                state.refresh_single_account(all_ids.front());
            } else if (!all_ids.empty()) {
                state.refresh_accounts_staggered(std::move(all_ids));
            }
            has_summary = true;

            path_buf = {};
            file_password.clear();
            queued.clear();
        }
    }

    if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
    }
    if (has_summary) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::Text("Imported %d new + updated %d existing across %d file(s) "
                    "(%d accounts read).",
                    summary.created, summary.merged, summary.files_ok,
                    summary.accounts_total);
        ImGui::PopStyleColor();
        if (summary.web_api_key_missing) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("No Web API key set: imported accounts will stay "
                               "blank until you add one in Settings and refresh.");
            ImGui::PopStyleColor();
        } else if (summary.blank_steam_id > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("%d account(s) couldn't fetch initial data "
                               "(no Steam ID resolved). Sign into Steam locally "
                               "or use Full login to populate them.",
                               summary.blank_steam_id);
            ImGui::PopStyleColor();
        }
        if (!summary.failures.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::Text("%zu file(s) failed:", summary.failures.size());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            for (const auto& f : summary.failures)
                ImGui::BulletText("%s", f.c_str());
            ImGui::PopStyleColor();
        }
    }
}

struct LoginWizard {
    enum class Phase { Credentials, GuardWait, Polling, Done, Error };
    Phase phase = Phase::Credentials;

    std::array<char, 64> username{};
    std::string password;
    std::string shared_secret;        // optional, base64; auto-generates Guard codes
    std::array<char, 8> guard_code{};

    std::string client_id;
    std::string request_id;
    std::uint64_t steam_id = 0;
    std::int64_t poll_interval = 5;
    std::vector<steam_login::GuardKind> allowed_guards;
    steam_login::GuardKind chosen_guard = steam_login::GuardKind::None;

    core::Account account;
    std::string status;
    std::string error;
    bool busy = false;
    bool poll_started = false;
    bool auto_guard_submitted = false;

    void reset() { *this = {}; }
};

widgets::StepperStep::State step_state(int step, int active_step) {
    if (step < active_step) return widgets::StepperStep::State::Done;
    if (step == active_step) return widgets::StepperStep::State::Active;
    return widgets::StepperStep::State::Pending;
}

void draw_full_login(app::AppState& state) {
    static LoginWizard wiz;
    auto* w = &wiz;

    // Background refresh signalled that this account needs a fresh login;
    // prefill the username field once and clear the signal.
    if (state.pending_relogin_login.has_value() &&
        wiz.phase == LoginWizard::Phase::Credentials) {
        const auto& login = *state.pending_relogin_login;
        std::snprintf(wiz.username.data(), wiz.username.size(), "%s", login.c_str());
        wiz.status = "Re-login required: refresh_token expired";
        state.pending_relogin_login.reset();
    }

    int active_step = 0;
    switch (wiz.phase) {
        case LoginWizard::Phase::Credentials: active_step = 0; break;
        case LoginWizard::Phase::GuardWait:   active_step = 1; break;
        case LoginWizard::Phase::Polling:     active_step = 2; break;
        case LoginWizard::Phase::Done:        active_step = 3; break;
        case LoginWizard::Phase::Error:       active_step = -1; break;
    }

    std::vector<widgets::StepperStep> steps{
        {"Credentials", step_state(0, active_step)},
        {"Steam Guard", step_state(1, active_step)},
        {"Tokens",      step_state(2, active_step)},
        {"Done",        step_state(3, active_step)},
    };
    if (wiz.phase == LoginWizard::Phase::Error) {
        for (auto& s : steps) {
            if (s.state == widgets::StepperStep::State::Active)
                s.state = widgets::StepperStep::State::Failed;
        }
    }
    widgets::draw_stepper(steps);

    ImGui::SameLine();
    ImGui::BeginChild("##login-body", ImVec2(0, 0), false);

    if (wiz.phase == LoginWizard::Phase::Credentials) {
        ImGui::SetNextItemWidth(280.0F);
        ImGui::InputText("Username", wiz.username.data(), wiz.username.size());
        hover_tooltip("Steam account name (lowercase username, not your persona).");
        widgets::draw_password_field("Password##login", wiz.password, false, 280.0F);
        hover_tooltip("Sent once to Steam's login endpoint to begin a mobile-confirmation "
                      "session. Stored encrypted in the vault on success.");
        widgets::draw_password_field("Shared secret (optional)##login-sda",
                                      wiz.shared_secret, false, 280.0F);
        hover_tooltip("Base64 shared_secret from your maFile. Optional: when present we "
                      "auto-generate the Steam Guard code and skip the manual code prompt.");
        ImGui::Spacing();

        const bool can_submit = wiz.username[0] != 0 && !wiz.password.empty() && !wiz.busy;
        ImGui::BeginDisabled(!can_submit);
        if (action_button("Sign in", ImVec2(120, 0))) {
            wiz.busy = true;
            wiz.status = "Requesting RSA key...";

            // Stash the optional shared_secret on the wizard's pending account
            // so the begin_session completion can auto-submit the Guard code
            // and so it's persisted alongside the tokens on success.
            if (!wiz.shared_secret.empty()) {
                core::SteamGuardAccount g;
                g.account_name = wiz.username.data();
                g.shared_secret = wiz.shared_secret;
                wiz.account.sda = std::move(g);
            } else {
                wiz.account.sda.reset();
            }

            std::string user(wiz.username.data());
            auto pw = crypto::make_secure(wiz.password);

            app::job_pump::submit([user, pw, w, &state] {
                steam_login::MobileLogin login;
                login.username = user;
                login.password = pw;
                auto result = steam_login::begin_session(login);

                std::lock_guard lk(state.job_mutex);
                state.completed_jobs.push_back({"", [w, user, pw, r = std::move(result)] {
                    w->busy = false;
                    if (!r.ok) {
                        w->error = r.error;
                        w->phase = LoginWizard::Phase::Error;
                        return;
                    }
                    w->client_id = r.client_id;
                    w->request_id = r.request_id;
                    w->steam_id = r.steam_id;
                    w->poll_interval = r.interval_seconds > 0 ? r.interval_seconds : 5;
                    w->allowed_guards = r.allowed_confirmations;

                    w->account.login = user;
                    w->account.password = pw;
                    w->account.steam_id_64 = r.steam_id;

                    bool needs_code = false;
                    for (auto k : w->allowed_guards) {
                        if (k == steam_login::GuardKind::DeviceCode ||
                            k == steam_login::GuardKind::EmailCode) {
                            w->chosen_guard = k;
                            needs_code = true;
                            break;
                        }
                    }

                    if (needs_code) {
                        w->phase = LoginWizard::Phase::GuardWait;
                        w->status = "Waiting for Steam Guard code...";
                    } else {
                        w->phase = LoginWizard::Phase::Polling;
                        w->poll_started = false;
                        w->status = "Polling for session tokens...";
                    }
                }});
            });
        }
        ImGui::EndDisabled();

    } else if (wiz.phase == LoginWizard::Phase::GuardWait) {
        // If we have a shared_secret + the prompt is for DeviceCode, generate
        // the TOTP and submit on the first frame in this phase so the user
        // never sees the manual prompt for a Steam-Guard-enrolled account.
        const bool can_auto_submit = !wiz.auto_guard_submitted &&
            wiz.chosen_guard == steam_login::GuardKind::DeviceCode &&
            wiz.account.sda.has_value() &&
            !wiz.account.sda->shared_secret.empty();

        if (can_auto_submit) {
            wiz.auto_guard_submitted = true;
            wiz.busy = true;
            wiz.status = "Generating Steam Guard code from shared_secret...";

            const std::string code = sam::sda::generate_code_now(
                wiz.account.sda->shared_secret);
            std::snprintf(wiz.guard_code.data(), wiz.guard_code.size(), "%s", code.c_str());

            std::string cid = wiz.client_id;
            std::uint64_t sid = wiz.steam_id;
            auto kind = wiz.chosen_guard;

            app::job_pump::submit([code, cid, sid, kind, w, &state] {
                std::string err;
                bool ok = steam_login::submit_guard_code(cid, sid, code, kind, &err);

                std::lock_guard lk(state.job_mutex);
                state.completed_jobs.push_back({"", [w, ok, err] {
                    w->busy = false;
                    if (!ok) {
                        // Auto-submitted code rejected; fall back to the
                        // manual prompt. Leave auto_guard_submitted=true so
                        // we don't re-submit the same (likely stale) code on
                        // every frame; the user can type a fresh one below.
                        w->status = "Auto-submitted code rejected: " + err;
                        return;
                    }
                    w->phase = LoginWizard::Phase::Polling;
                    w->poll_started = false;
                    w->status = "Polling for session tokens...";
                }});
            });
        }

        if (wiz.chosen_guard == steam_login::GuardKind::EmailCode) {
            ImGui::TextWrapped("A Steam Guard code was sent to your email.");
        } else if (can_auto_submit || wiz.auto_guard_submitted) {
            ImGui::TextWrapped("Submitting Steam Guard code from shared_secret...");
        } else {
            ImGui::TextWrapped("Enter the code from your Steam Mobile authenticator.");
        }
        ImGui::Spacing();
        ImGui::SetNextItemWidth(120.0F);
        ImGui::InputText("Guard code", wiz.guard_code.data(), wiz.guard_code.size());
        hover_tooltip("The 5-character Steam Guard code (from email or the official mobile app).");
        ImGui::Spacing();

        const bool can_submit = wiz.guard_code[0] != 0 && !wiz.busy;
        ImGui::BeginDisabled(!can_submit);
        if (action_button("Submit code", ImVec2(120, 0))) {
            wiz.busy = true;
            wiz.status = "Submitting guard code...";

            std::string code(wiz.guard_code.data());
            std::string cid = wiz.client_id;
            std::uint64_t sid = wiz.steam_id;
            auto kind = wiz.chosen_guard;

            app::job_pump::submit([code, cid, sid, kind, w, &state] {
                std::string err;
                bool ok = steam_login::submit_guard_code(cid, sid, code, kind, &err);

                std::lock_guard lk(state.job_mutex);
                state.completed_jobs.push_back({"", [w, ok, err] {
                    w->busy = false;
                    if (!ok) {
                        w->error = "Guard code rejected: " + err;
                        w->phase = LoginWizard::Phase::Error;
                        return;
                    }
                    w->phase = LoginWizard::Phase::Polling;
                    w->poll_started = false;
                    w->status = "Polling for session tokens...";
                }});
            });
        }
        ImGui::EndDisabled();

    } else if (wiz.phase == LoginWizard::Phase::Polling) {
        if (!wiz.poll_started) {
            wiz.poll_started = true;
            wiz.busy = true;

            std::string cid = wiz.client_id;
            std::string rid = wiz.request_id;
            auto interval = wiz.poll_interval;
            auto sid = wiz.steam_id;

            app::job_pump::submit([cid, rid, interval, sid, w, &state] {
                std::string local_cid = cid;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

                while (std::chrono::steady_clock::now() < deadline) {
                    auto poll = steam_login::poll_session(local_cid, rid);
                    if (!poll.ok) {
                        std::lock_guard lk(state.job_mutex);
                        state.completed_jobs.push_back({"", [w, e = poll.error] {
                            if (w->phase != LoginWizard::Phase::Polling) return;
                            w->busy = false;
                            w->error = e;
                            w->phase = LoginWizard::Phase::Error;
                        }});
                        return;
                    }
                    if (poll.finished) {
                        auto expiry = steam_login::jwt_expiry(poll.access_token);
                        auto login_secure = crypto::make_secure(
                            steam_login::make_steam_login_secure(sid, poll.access_token));

                        // Register the session in Steam's community session
                        // table so the freshly-added account can immediately
                        // accept confirmations without needing an auto-relogin
                        // recovery. sess_id is the one bound to the resulting
                        // cookie and must be the sessionid we store on the
                        // account; mobileconf will send it back as a cookie
                        // and Steam matches it to the registered session.
                        std::string sess_id = crypto::random_session_id();
                        std::string registered_cookie;
                        if (steam_login::transfer_login(sid, poll.refresh_token,
                                                         sess_id, registered_cookie)) {
                            login_secure = crypto::make_secure(registered_cookie);
                        } else {
                            SAM_LOG_WARN("add_account: transfer_login failed for '{}'; "
                                         "mobileconf writes will need a relogin",
                                         w->account.login);
                        }

                        std::lock_guard lk(state.job_mutex);
                        state.completed_jobs.push_back({"", [w, &state,
                                at = std::move(poll.access_token),
                                rt = std::move(poll.refresh_token),
                                expiry, ls = std::move(login_secure),
                                sess_id = std::move(sess_id)] {
                            if (w->phase != LoginWizard::Phase::Polling) return;

                            // Merge into an existing vault entry if one matches by
                            // steam_id or login; otherwise create a new account.
                            // Avoids duplicates when re-logging into an already-
                            // imported account (e.g. one added via maFile with a
                            // different login casing).
                            core::Account* existing = core::store::find_existing_account(
                                state.vault, w->account.steam_id_64, w->account.login);

                            std::string id_for_refresh;
                            if (existing) {
                                existing->password         = w->account.password;
                                existing->steam_id_64      = w->account.steam_id_64;
                                existing->access_token     = at;
                                existing->refresh_token    = rt;
                                existing->access_token_expires = expiry;
                                existing->steam_login_secure   = ls;
                                // The sessionid we used at finalizelogin is
                                // the one bound to ls; mobileconf rejects
                                // the cookie if the sessionid we send back
                                // does not match the registered session.
                                existing->session_id = sess_id;
                                existing->last_login_unix = now_seconds();
                                id_for_refresh = existing->id;
                                // If the user supplied a shared_secret on this
                                // login attempt, carry it over. Don't blow
                                // away a previously-imported maFile when the
                                // user re-runs Full Login without it.
                                if (w->account.sda.has_value() &&
                                    !w->account.sda->shared_secret.empty()) {
                                    if (!existing->sda.has_value()) {
                                        existing->sda = w->account.sda;
                                    } else {
                                        existing->sda->shared_secret =
                                            w->account.sda->shared_secret;
                                        if (existing->sda->account_name.empty())
                                            existing->sda->account_name =
                                                w->account.sda->account_name;
                                    }
                                }
                                w->status = "Existing account updated with fresh tokens.";
                            } else {
                                w->account.id = generate_ulid();
                                w->account.access_token = at;
                                w->account.refresh_token = rt;
                                w->account.access_token_expires = expiry;
                                w->account.steam_login_secure = ls;
                                w->account.session_id = sess_id;
                                w->account.created_unix = now_seconds();
                                w->account.last_login_unix = now_seconds();
                                id_for_refresh = w->account.id;
                                state.vault.accounts.push_back(std::move(w->account));
                                w->status = "Login successful.";
                            }
                            state.vault_dirty = true;
                            state.save_vault_if_dirty();

                            // Populate avatar / level / games / CS2 rank so the
                            // newly-saved card isn't blank when the user clicks
                            // "Go to Accounts".
                            if (!id_for_refresh.empty()) {
                                state.refresh_single_account(id_for_refresh);
                            }

                            w->busy = false;
                            w->phase = LoginWizard::Phase::Done;
                        }});
                        return;
                    }
                    if (!poll.new_client_id.empty()) {
                        local_cid = poll.new_client_id;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::seconds(interval > 0 ? interval : 5));
                }

                std::lock_guard lk(state.job_mutex);
                state.completed_jobs.push_back({"", [w] {
                    if (w->phase != LoginWizard::Phase::Polling) return;
                    w->busy = false;
                    w->error = "Login timed out after 2 minutes.";
                    w->phase = LoginWizard::Phase::Error;
                }});
            });
        }

        ImGui::TextWrapped("Waiting for Steam to confirm the session...");
        ImGui::Spacing();
        // Lets the user bail out before the 2-minute deadline. The poll loop
        // checks `phase != Polling` on every completed_jobs callback, so
        // flipping the phase here is enough; the worker exits on its next
        // post-back attempt and the wizard stays out of its way.
        if (action_button("Cancel", ImVec2(120, 0))) {
            wiz.busy = false;
            wiz.error = "Login cancelled.";
            wiz.phase = LoginWizard::Phase::Error;
        }

    } else if (wiz.phase == LoginWizard::Phase::Done) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::TextWrapped("Account authenticated and saved to vault.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (action_button("Go to Accounts", ImVec2(160, 0))) {
            wiz.reset();
            state.current_screen = app::Screen::Accounts;
        }

    } else if (wiz.phase == LoginWizard::Phase::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("%s", wiz.error.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (action_button("Try again", ImVec2(120, 0))) {
            wiz.reset();
        }
    }

    if (!wiz.status.empty() && wiz.busy) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextUnformatted(wiz.status.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

void draw_import_bundle(app::AppState& state) {
    static widgets::ImportBundleState import_state;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
    ImGui::TextWrapped(
        "Import accounts from an encrypted .samvault bundle exported on this "
        "or another machine. The bundle has its own passphrase, separate from "
        "your master password. Matching accounts (by Steam ID, otherwise by "
        "login) are overwritten in place; new accounts are appended.");
    ImGui::PopStyleColor();

    ImGui::Spacing();

    if (action_button("Choose bundle file...")) {
        platform::file_dialog::Options opts;
        opts.parent = state.main_hwnd;
        opts.title = L"Import bundle";
        opts.filters = {
            {L"Steam Account Manager bundle (*.samvault)", L"*.samvault"},
            {L"All files (*.*)", L"*.*"},
        };
        const auto picked = platform::file_dialog::open_file(opts);
        if (picked.ok) {
            widgets::request_import_bundle(import_state, picked.path);
        }
    }

    widgets::draw_import_bundle_modal(state, import_state);
}

}  // namespace

void draw_add_account(app::AppState& state) {
    core::Account* editing = nullptr;
    if (!state.selected_account_id.empty())
        editing = state.find_account(state.selected_account_id);

    ImGui::TextUnformatted(editing ? "Edit account" : "Add account");
    ImGui::Spacing();

    if (editing) {
        draw_manual(state, editing);
    } else {
        const bool force_full_login = state.pending_relogin_login.has_value();
        bool force_mafile = false;
        bool force_info_dat = false;
        {
            std::lock_guard lk(state.drop_mutex);
            if (state.pending_mafile_focus) {
                force_mafile = true;
                state.pending_mafile_focus = false;
            }
            if (state.pending_info_dat_focus) {
                force_info_dat = true;
                state.pending_info_dat_focus = false;
            }
        }
        if (ImGui::BeginTabBar("##add-tabs")) {
            if (ImGui::BeginTabItem("Manual")) {
                draw_manual(state, nullptr);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags mafile_flags = ImGuiTabItemFlags_None;
            if (force_mafile) mafile_flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem("Import maFile", nullptr, mafile_flags)) {
                draw_import_mafile(state);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags info_dat_flags = ImGuiTabItemFlags_None;
            if (force_info_dat) info_dat_flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem("Import info.dat", nullptr, info_dat_flags)) {
                draw_import_info_dat(state);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags full_flags = ImGuiTabItemFlags_None;
            if (force_full_login) full_flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem("Full login", nullptr, full_flags)) {
                draw_full_login(state);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Import bundle")) {
                draw_import_bundle(state);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
}

}  // namespace sam::ui::screens
