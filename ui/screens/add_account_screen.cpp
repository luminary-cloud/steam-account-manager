#include "ui/screens/add_account_screen.hpp"

#include "ui/screens/add_account_detail.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
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
#include "platform/clipboard.hpp"
#include "platform/file_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/bundle_dialogs.hpp"
#include "ui/widgets/master_pw_field.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/stepper.hpp"

namespace sam::ui::screens {

namespace add_account_detail {

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

// Explorer's "copy as path" wraps the path in quotes; strip those and surrounding space.
std::string trim_path(std::string s) {
    while (!s.empty() && (s.front() == '"' || s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == '"' || s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

// Non-recursive; ext_lower must include the leading dot and be lowercase.
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

}  // namespace add_account_detail

namespace {

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

    separator_text("Account credentials");

    const bool editing_redacted =
        editing && state.settings.privacy_mode &&
        state.revealed_logins.find(editing->id) == state.revealed_logins.end();
    if (editing_redacted) {
        // Clickable redacted label stands in for the InputText; the buffer above
        // still holds the real login so save_changes works unchanged.
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
            if (editing->steam_id_64 == 0 && !editing->login.empty()) {
                editing->steam_id_64 = steam_local::lookup_steam_id(editing->login);
                if (editing->steam_id_64 != 0) new_id_for_refresh = editing->id;
            }
        } else {
            core::Account a;
            a.id = add_account_detail::generate_ulid();
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

// NFA accounts have no password; editing one shows its token (with reveal/copy and a
// replace field) instead of the password-oriented manual form.
void draw_edit_nfa_token(app::AppState& state, core::Account* editing) {
    static std::string notes;
    static std::array<char, 4096> replace_buf{};
    static std::string replace_error;
    static std::string loaded_id;
    static std::unordered_map<std::string, std::int64_t> reveal_until;  // id -> unix

    const std::string acc_id = editing->id;
    const std::int64_t now = now_seconds();

    if (loaded_id != acc_id) {
        notes = to_utf8(editing->notes);
        replace_buf = {};
        replace_error.clear();
        reveal_until[acc_id] = 0;  // start masked when switching accounts
        loaded_id = acc_id;
    }

    separator_text("NFA account");

    // Login (read-only, privacy-aware) — mirrors the manual form.
    const bool editing_redacted =
        state.settings.privacy_mode &&
        state.revealed_logins.find(acc_id) == state.revealed_logins.end();
    if (editing_redacted) {
        widgets::draw_login_text(state, *editing);
        ImGui::SameLine();
        ImGui::TextUnformatted("Login");
    } else {
        ImGui::Text("Login: %s",
                    editing->login.empty() ? "(unknown)" : editing->login.c_str());
    }

    if (editing->steam_id_64 != 0) {
        ImGui::Text("Steam ID: %llu",
                    static_cast<unsigned long long>(editing->steam_id_64));
        ImGui::SameLine();
        if (action_button("Copy##nfa-sid"))
            platform::clipboard::set_text(std::to_string(editing->steam_id_64));
    } else {
        ImGui::TextDisabled("Steam ID: unresolved");
    }

    // Expiry from the cached value, decoding the token only as a fallback.
    std::int64_t exp = editing->refresh_token_expires;
    if (exp == 0 && !editing->refresh_token.empty())
        exp = steam_login::jwt_expiry(editing->refresh_token);
    if (exp != 0) {
        char when[64] = {};
        const auto t = static_cast<std::time_t>(exp);
        std::tm tm{};
        gmtime_s(&tm, &t);
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", &tm);
        if (exp <= now) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::Text("Token expired on %s", when);
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::Text("Token valid until %s (%lld day(s) left)", when,
                        static_cast<long long>((exp - now) / 86400));
            ImGui::PopStyleColor();
        }
    }

    separator_text("NFA token");
    std::string token_plain;
    if (!editing->refresh_token.empty())
        token_plain.assign(editing->refresh_token.begin(), editing->refresh_token.end());
    std::string full_token = editing->steam_id_64 != 0
        ? std::to_string(editing->steam_id_64) + "----" + token_plain
        : token_plain;

    const bool revealed = reveal_until[acc_id] > now;
    if (editing->refresh_token.empty()) {
        ImGui::TextDisabled("No token stored. Paste one below to set it.");
    } else if (revealed) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(full_token.c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextDisabled("(hidden - click Reveal)");
    }

    if (!editing->refresh_token.empty()) {
        if (action_button(revealed ? "Hide" : "Reveal"))
            reveal_until[acc_id] = revealed ? 0 : (now + 30);
        ImGui::SameLine();
        if (action_button("Copy token")) {
            platform::clipboard::set_text_with_auto_clear(
                full_token, std::chrono::seconds(state.settings.clipboard_clear_seconds));
        }
        hover_tooltip("Copies the full SteamID----JWT token; the clipboard auto-clears.");
    }

    separator_text("Replace token");
    ImGui::TextDisabled("Paste a fresh SteamID----JWT to update this account's token.");
    ImGui::InputTextMultiline("##nfa-replace", replace_buf.data(), replace_buf.size(),
                              ImVec2(440.0F, 70.0F));
    ImGui::BeginDisabled(replace_buf[0] == 0);
    if (action_button("Replace token")) {
        replace_error.clear();
        const add_account_detail::JwtImportResult r =
            add_account_detail::import_jwt_token(state, replace_buf.data());
        if (!r.ok) {
            replace_error = r.error;
        } else {
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            state.nfa_dead_notified.erase(r.account_id);
            state.refresh_single_account(r.account_id);
            // import_jwt_token may have grown the vault vector, invalidating `editing`;
            // force a buffer reload next frame and stop touching `editing` now.
            loaded_id.clear();
            ImGui::EndDisabled();
            return;
        }
    }
    ImGui::EndDisabled();
    if (!replace_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped("%s", replace_error.c_str());
        ImGui::PopStyleColor();
    }

    separator_text("Notes");
    {
        std::array<char, 1024> nbuf{};
        std::snprintf(nbuf.data(), nbuf.size(), "%s", notes.c_str());
        if (ImGui::InputTextMultiline("##nfa-notes", nbuf.data(), nbuf.size(),
                                       ImVec2(440.0F, 80.0F))) {
            notes = nbuf.data();
        }
    }

    ImGui::Spacing();
    if (action_button("Save changes")) {
        editing->notes = to_wide(notes);
        state.vault_dirty = true;
        state.save_vault_if_dirty();
        loaded_id.clear();
        state.selected_account_id.clear();
        state.current_screen = app::Screen::Accounts;
    }
    ImGui::SameLine();
    if (action_button("Done")) {
        loaded_id.clear();
        state.selected_account_id.clear();
        state.current_screen = app::Screen::Accounts;
    }
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
        if (editing->is_nfa) draw_edit_nfa_token(state, editing);
        else                 draw_manual(state, editing);
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
                add_account_detail::draw_import_mafile(state);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags info_dat_flags = ImGuiTabItemFlags_None;
            if (force_info_dat) info_dat_flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem("Import info.dat", nullptr, info_dat_flags)) {
                add_account_detail::draw_import_info_dat(state);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("NFA token")) {
                add_account_detail::draw_import_jwt_token(state);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags full_flags = ImGuiTabItemFlags_None;
            if (force_full_login) full_flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem("Full login", nullptr, full_flags)) {
                add_account_detail::draw_full_login(state);
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
