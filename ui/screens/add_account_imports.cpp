#include "ui/screens/add_account_detail.hpp"

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

struct MafileImportResult {
    bool ok = false;
    bool merged = false;          // existing account updated rather than created
    std::string error;
    std::string account_id;
    std::string login;
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
        // Some SDA exports omit Session.SteamID; the access_token's sub claim is the steam_id_64.
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
            a.id = add_account_detail::generate_ulid();
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

struct InfoDatImportResult {
    bool ok = false;
    int  created = 0;
    int  merged = 0;
    int  accounts_in_file = 0;
    std::string error;
    std::string file_label;
    std::vector<std::string> account_ids;
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

                // Ban snapshot from the file; overwritten by the first online refresh,
                // but lets VAC/etc pills show immediately.
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
                a.id = add_account_detail::generate_ulid();
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

struct MafileBatchSummary {
    int imported = 0;
    int merged = 0;
    int blank_steam_id = 0;              // imported but no steam_id_64 resolved
    bool web_api_key_missing = false;
    std::vector<std::string> failures;   // "<filename>: <error>"
};

struct InfoDatBatchSummary {
    int files_ok = 0;
    int created  = 0;
    int merged   = 0;
    int accounts_total = 0;
    int blank_steam_id = 0;             // imported but no steam_id_64 resolved
    bool web_api_key_missing = false;
    std::vector<std::string> failures;  // "<filename>: <error>"
};

}  // namespace

namespace add_account_detail {

struct LoginToken {
    std::string login;
    std::string token;
};

// Splits "<login>----<token>" (the form NFA exports use) or a bare token, and drops
// any trailing "----key:value" metadata some exporters append after the JWT. The
// token is the field between the first and second "----" (a JWT never contains it).
LoginToken split_login_token(std::string raw) {
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.pop_back();
        return s;
    };
    raw = trim(std::move(raw));
    const auto first = raw.find("----");
    if (first == std::string::npos) return {std::string{}, raw};
    std::string login = trim(raw.substr(0, first));
    std::string rest = raw.substr(first + 4);
    const auto second = rest.find("----");
    std::string token = (second == std::string::npos) ? rest : rest.substr(0, second);
    return {std::move(login), trim(std::move(token))};
}

JwtImportResult import_jwt_token(app::AppState& state, const std::string& raw) {
    JwtImportResult r;
    LoginToken lt = split_login_token(raw);
    std::string login_hint = lt.login;
    if (lt.token.empty()) { r.error = "No token provided."; return r; }

    auto rt = crypto::make_secure(lt.token);
    const std::int64_t exp = steam_login::jwt_expiry(rt);
    if (exp == 0) {
        r.error = "That doesn't look like a valid JWT (couldn't read its expiry).";
        return r;
    }
    const std::string aud = steam_login::jwt_audience(rt);
    r.client_audience = aud.find("client") != std::string::npos;
    r.steam_issuer = steam_login::jwt_issuer(rt).find("steam") != std::string::npos;
    r.expires = exp;
    r.expired = exp <= now_seconds();

    std::uint64_t sid = steam_login::jwt_steam_id(rt);
    if (sid == 0 && !login_hint.empty()) sid = steam_local::lookup_steam_id(login_hint);
    r.steam_id = sid;

    const std::string group_id = core::store::ensure_nfa_group(state.vault);

    auto apply = [&](core::Account& a, bool fresh) {
        a.refresh_token = rt;
        a.refresh_token_expires = exp;
        if (sid != 0) a.steam_id_64 = sid;
        if (!login_hint.empty()) a.login = login_hint;
        if (a.session_id.empty()) a.session_id = crypto::random_session_id();
        if (fresh) a.created_unix = now_seconds();

        // No stored password = NFA (launches via token injection, lands in the NFA group).
        // Importing a token onto a full-access account just stores it, type unchanged.
        const bool nfa = a.password.empty();
        a.is_nfa = nfa;
        if (nfa) a.group_id = group_id;
    };

    core::Account* existing = core::store::find_existing_account(state.vault, sid, login_hint);
    if (existing) {
        apply(*existing, false);
        r.account_id = existing->id;
        r.login = existing->login;
        r.merged = true;
    } else {
        core::Account a;
        a.id = add_account_detail::generate_ulid();
        apply(a, true);
        r.account_id = a.id;
        r.login = a.login;
        state.vault.accounts.push_back(std::move(a));
    }
    r.ok = true;
    return r;
}

void draw_import_mafile(app::AppState& state) {
    static std::array<char, 1024> path_buf{};
    static std::string mafile_password;
    static std::string steam_password;
    static std::vector<std::string> queued;       // from drag-drop
    static MafileBatchSummary summary;
    static bool has_summary = false;
    static std::string error;

    // Drain the WM_DROPFILES queue.
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

    separator_text("Steam Mobile Authenticator file");
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
                    // No resolved steam_id_64 means the account won't auto-refresh and
                    // shows as a blank card until one is provided.
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
            // Stagger bulk refreshes so a big import doesn't burst the Web API.
            if (account_ids.size() == 1) {
                state.refresh_single_account(account_ids.front());
            } else if (!account_ids.empty()) {
                state.refresh_accounts_staggered(std::move(account_ids));
            }
            has_summary = true;

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

    separator_text("info.dat import");
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
            // info.dat files can carry many accounts each; stagger to avoid a Web API burst.
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

void draw_import_jwt_token(app::AppState& state) {
    static std::array<char, 4096> token_buf{};
    static JwtImportResult result;
    static bool has_result = false;
    static std::string error;

    separator_text("NFA token import");
    ImGui::TextDisabled("Paste a Steam refresh token as username----token (a bare token works too, "
                        "but then Login can't resolve the account name).");

    ImGui::TextUnformatted("Token");
    ImGui::InputTextMultiline("##nfa-token", token_buf.data(), token_buf.size(),
                              ImVec2(440.0F, 80.0F));
    hover_tooltip("Format: username----eyA...  The username before the dashes is the Steam account "
                  "name (needed for Login); the rest is the JWT refresh token, stored encrypted. An "
                  "NFA account has no password; this token is its only credential.");

    if (state.settings.web_api_key.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextWrapped("No Steam Web API key set. Stats (level, games, bans) won't populate "
                           "until you add one in Settings.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(token_buf[0] == 0);
    if (action_button("Import##nfa-import")) {
        error.clear();
        has_result = false;
        result = import_jwt_token(state, token_buf.data());
        if (!result.ok) {
            error = result.error;
        } else {
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            state.nfa_dead_notified.erase(result.account_id);
            state.refresh_single_account(result.account_id);
            has_result = true;
            token_buf = {};
        }
    }
    ImGui::EndDisabled();

    if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
    }
    if (has_result) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::Text("%s %s (Steam ID %llu).",
                    result.merged ? "Updated" : "Imported",
                    result.login.empty() ? "(login unknown)" : result.login.c_str(),
                    static_cast<unsigned long long>(result.steam_id));
        ImGui::PopStyleColor();

        if (result.expired) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("This token is expired. The account was added but Login won't work "
                               "until you import a fresh token.");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::Text("Token valid for %lld day(s).",
                        static_cast<long long>((result.expires - now_seconds()) / 86400));
            ImGui::PopStyleColor();
        }
        if (!result.client_audience) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("Heads up: this token's audience doesn't include \"client\", so the "
                               "Steam client may reject it at Login.");
            ImGui::PopStyleColor();
        }
        if (!result.steam_issuer) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("Heads up: this token's issuer isn't Steam, so it may not be a valid "
                               "Steam refresh token.");
            ImGui::PopStyleColor();
        }
        if (result.login.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextWrapped("No account name resolved. Set one (edit the account, or re-import "
                               "as username----token) so Login can sign in.");
            ImGui::PopStyleColor();
        }
    }
}

}  // namespace add_account_detail

}  // namespace sam::ui::screens
