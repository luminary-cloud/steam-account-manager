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

    static std::atomic<std::uint64_t> counter{0};
    const auto n = counter.fetch_add(1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx%08llx",
                  static_cast<unsigned long long>(now_seconds()),
                  static_cast<unsigned long long>(n));
    return buf;
}

std::string trim_path(std::string s) {
    while (!s.empty() && (s.front() == '"' || s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == '"' || s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

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

std::string full_login_token(const core::Account& a, const std::string& jwt) {
    if (jwt.empty()) return {};
    if (!a.login.empty()) return a.login + "----" + jwt;
    if (a.steam_id_64 != 0) return std::to_string(a.steam_id_64) + "----" + jwt;
    return jwt;
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

void draw_edit_notes_modal(app::AppState& state) {
    static std::array<char, 1024> notes_buf{};

    if (state.notes_edit_requested) {
        state.notes_edit_requested = false;
        notes_buf.fill('\0');
        if (const auto* acc = state.find_account(state.selected_account_id)) {
            std::snprintf(notes_buf.data(), notes_buf.size(), "%s",
                          to_utf8(acc->notes).c_str());
        }
        ImGui::OpenPopup("Edit notes");
    }

    if (!begin_styled_modal("Edit notes", 420.0F)) return;

    core::Account* acc = state.find_account(state.selected_account_id);
    if (acc == nullptr) {
        ImGui::TextDisabled("Account no longer exists.");
        if (action_button("Close", ImVec2(80, 0))) ImGui::CloseCurrentPopup();
        end_styled_modal();
        return;
    }

    ImGui::TextUnformatted("Notes");
    ImGui::InputTextMultiline("##edit-notes", notes_buf.data(), notes_buf.size(),
                              ImVec2(-1.0F, 110.0F));

    ImGui::Spacing();
    if (action_button("Save", ImVec2(100, 0))) {
        acc->notes = to_wide(notes_buf.data());
        state.vault_dirty = true;
        state.save_vault_if_dirty();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (action_button("Cancel", ImVec2(100, 0))) {
        ImGui::CloseCurrentPopup();
    }
    end_styled_modal();
}

void draw_edit_account_modal(app::AppState& state) {
    constexpr const char* kPwLabel = "Password##edit";
    constexpr const char* kSsLabel = "Shared secret (optional)##edit-sda";

    static std::array<char, 64> login_buf{};
    static std::string password;
    static std::string shared_secret;
    static std::string notes;
    static std::array<char, 4096> replace_buf{};
    static std::string replace_error;
    static std::int64_t reveal_until = 0;
    static std::string mint_account_id;
    static std::string mint_error;

    if (state.account_edit_requested) {
        state.account_edit_requested = false;
        login_buf.fill('\0');
        password.clear();
        shared_secret.clear();
        notes.clear();
        replace_buf.fill('\0');
        replace_error.clear();
        reveal_until = 0;
        mint_error.clear();
        widgets::reset_password_visibility(kPwLabel);
        widgets::reset_password_visibility(kSsLabel);
        if (const auto* acc = state.find_account(state.selected_account_id)) {
            std::snprintf(login_buf.data(), login_buf.size(), "%s", acc->login.c_str());
            password = std::string(acc->password.begin(), acc->password.end());
            shared_secret = acc->sda.has_value() ? acc->sda->shared_secret : std::string{};
            notes = to_utf8(acc->notes);
        }
        ImGui::OpenPopup("Edit account");
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    if (!begin_styled_modal("Edit account", 540.0F)) return;

    core::Account* acc = state.find_account(state.selected_account_id);
    if (acc == nullptr) {
        ImGui::TextDisabled("Account no longer exists.");
        if (action_button("Close", ImVec2(80, 0))) ImGui::CloseCurrentPopup();
        end_styled_modal();
        return;
    }

    const std::int64_t now = now_seconds();

    if (acc->is_nfa) {

        separator_text("Account");
        ImGui::Text("Login: %s",
                    acc->login.empty() ? "(unknown)" : acc->login.c_str());
        if (acc->steam_id_64 != 0) {
            ImGui::Text("Steam ID: %llu",
                        static_cast<unsigned long long>(acc->steam_id_64));
        }

        std::string token_plain;
        if (!acc->refresh_token.empty())
            token_plain.assign(acc->refresh_token.begin(), acc->refresh_token.end());
        const std::string full_token = full_login_token(*acc, token_plain);

        separator_text("NFA token");
        const bool revealed = reveal_until > now;
        if (acc->refresh_token.empty()) {
            ImGui::TextDisabled("No token stored. Paste one below to set it.");
        } else if (revealed) {
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted(full_token.c_str());
            ImGui::PopTextWrapPos();
        } else {
            ImGui::TextDisabled("(hidden - click Reveal)");
        }
        if (!acc->refresh_token.empty()) {
            if (action_button(revealed ? "Hide" : "Reveal"))
                reveal_until = revealed ? 0 : (now + 30);
            ImGui::SameLine();
            if (action_button("Copy token")) {
                platform::clipboard::set_text_with_auto_clear(
                    full_token,
                    std::chrono::seconds(state.settings.clipboard_clear_seconds));
            }
            hover_tooltip("Copies the full login----JWT token; the clipboard auto-clears.");
        }

        separator_text("Replace token");
        ImGui::TextDisabled("Paste a fresh SteamID----JWT to update this account's token.");
        ImGui::InputTextMultiline("##nfa-replace", replace_buf.data(), replace_buf.size(),
                                  ImVec2(-1.0F, 70.0F));
        ImGui::BeginDisabled(replace_buf[0] == 0);
        if (action_button("Replace token")) {
            replace_error.clear();
            const add_account_detail::JwtImportResult r =
                add_account_detail::import_jwt_token(
                    state, replace_buf.data(), !core::store::is_cached_account(*acc));
            if (!r.ok) {
                replace_error = r.error;
            } else {
                state.vault_dirty = true;
                state.save_vault_if_dirty();
                state.nfa_dead_notified.erase(r.account_id);
                state.pull_all_for_account(r.account_id);

                state.selected_account_id = r.account_id;
                state.account_edit_requested = true;
                ImGui::EndDisabled();
                end_styled_modal();
                return;
            }
        }
        ImGui::EndDisabled();
        if (!replace_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextWrapped("%s", replace_error.c_str());
            ImGui::PopStyleColor();
        }

        if (core::store::is_cached_account(*acc)) {
            separator_text("Account type");
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Imported from this PC. Convert it to a full-access account "
                               "to add a password and use it like a normally added account.");
            ImGui::PopStyleColor();
            if (action_button("Convert to full access")) {
                acc->is_nfa = false;
                auto& tids = acc->tag_ids;
                tids.erase(std::remove(tids.begin(), tids.end(),
                                       std::string(core::store::kCachedTagId)),
                           tids.end());
                if (acc->group_id == core::store::kCachedGroupId) acc->group_id.clear();
                state.vault_dirty = true;
                state.save_vault_if_dirty();

                state.selected_account_id = acc->id;
                state.account_edit_requested = true;
                end_styled_modal();
                return;
            }
        }

        separator_text("Notes");
        {
            std::array<char, 1024> nbuf{};
            std::snprintf(nbuf.data(), nbuf.size(), "%s", notes.c_str());
            if (ImGui::InputTextMultiline("##nfa-notes", nbuf.data(), nbuf.size(),
                                           ImVec2(-1.0F, 80.0F))) {
                notes = nbuf.data();
            }
        }

        ImGui::Spacing();
        if (action_button("Save changes", ImVec2(110, 0))) {
            acc->notes = to_wide(notes);
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (action_button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
        return;
    }

    separator_text("Account credentials");

    ImGui::SetNextItemWidth(240.0F);
    ImGui::InputText("Username", login_buf.data(), login_buf.size());
    hover_tooltip("The lowercase login name, not the persona. Renaming is local only.");

    widgets::draw_password_field(kPwLabel, password, false, 240.0F);
    hover_tooltip("Stored encrypted. Used by Launch to fill Steam's prompt.");

    widgets::draw_password_field(kSsLabel, shared_secret, false, 240.0F);
    hover_tooltip("Base64 shared_secret from your maFile. Optional, but needed for "
                  "in-app Steam Guard codes.");

    separator_text("Login token");
    {
        const std::string jwt = app::cs2_client_token(*acc);
        if (jwt.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("No login token stored yet. One is minted the first time this "
                               "account signs in through token injection, or now from the "
                               "stored password.");
            ImGui::PopStyleColor();

            const bool minting = mint_account_id == acc->id;
            const bool can_mint = !acc->password.empty();
            ImGui::BeginDisabled(minting || !can_mint);
            if (action_button("Mint token")) {
                mint_account_id = acc->id;
                mint_error.clear();
                state.acquire_cm_token(acc->id, [](bool ok, std::string err) {
                    mint_account_id.clear();
                    if (!ok) mint_error = std::move(err);
                });
            }
            ImGui::EndDisabled();
            if (!can_mint && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                set_tooltip("Minting needs a stored password. This account has none.");
            }
            if (minting) {
                ImGui::SameLine();
                ImGui::TextDisabled("Signing in...");
            }
            if (!mint_error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
                ImGui::TextWrapped("%s", mint_error.c_str());
                ImGui::PopStyleColor();
            }
        } else {
            const std::string full_token = full_login_token(*acc, jwt);
            const bool revealed = reveal_until > now;
            if (revealed) {
                ImGui::PushTextWrapPos(0.0F);
                ImGui::TextUnformatted(full_token.c_str());
                ImGui::PopTextWrapPos();
            } else {
                ImGui::TextDisabled("(hidden - click Reveal)");
            }
            if (action_button(revealed ? "Hide" : "Reveal"))
                reveal_until = revealed ? 0 : (now + 30);
            ImGui::SameLine();
            if (action_button("Copy token")) {
                platform::clipboard::set_text_with_auto_clear(
                    full_token,
                    std::chrono::seconds(state.settings.clipboard_clear_seconds));
            }
            hover_tooltip("Copies the full login----JWT token. Steam rotates it on every "
                          "sign-in, so a copy goes stale once this PC signs in again.");

            const std::int64_t exp = steam_login::jwt_expiry(crypto::make_secure(jwt));
            if (exp > 0) {
                const std::int64_t remaining = exp - now;
                if (remaining <= 0) {
                    ImGui::TextDisabled("Expired.");
                } else if (remaining < 86400) {
                    ImGui::TextDisabled("Expires in %lldh",
                                        static_cast<long long>(remaining / 3600));
                } else {
                    ImGui::TextDisabled("Expires in %lldd",
                                        static_cast<long long>(remaining / 86400));
                }
            }
            if (acc->cm_status == core::NfaTokenStatus::Revoked) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
                ImGui::TextWrapped("Steam refused this token on the last sign-in. It is "
                                   "re-minted automatically on the next launch.");
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::TextUnformatted("Notes");
    {
        std::array<char, 1024> nbuf{};
        std::snprintf(nbuf.data(), nbuf.size(), "%s", notes.c_str());
        if (ImGui::InputTextMultiline("##edit-account-notes", nbuf.data(), nbuf.size(),
                                       ImVec2(-1.0F, 90.0F))) {
            notes = nbuf.data();
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(login_buf[0] == 0);
    if (action_button("Save changes", ImVec2(110, 0))) {
        std::string new_id_for_refresh;
        const std::string old_login = acc->login;
        acc->login = login_buf.data();
        acc->password = crypto::make_secure(password);
        acc->notes = to_wide(notes);
        if (!shared_secret.empty()) {
            if (!acc->sda.has_value()) {
                core::SteamGuardAccount g;
                g.account_name = acc->login;
                acc->sda = std::move(g);
            }
            acc->sda->shared_secret = shared_secret;

            if (acc->sda->account_name.empty() || acc->sda->account_name == old_login)
                acc->sda->account_name = acc->login;
        }
        if (acc->steam_id_64 == 0 && !acc->login.empty()) {
            acc->steam_id_64 = steam_local::lookup_steam_id(acc->login);
            if (acc->steam_id_64 != 0) new_id_for_refresh = acc->id;
        }
        state.vault_dirty = true;
        state.save_vault_if_dirty();
        if (!new_id_for_refresh.empty()) state.pull_all_for_account(new_id_for_refresh);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (action_button("Cancel", ImVec2(100, 0))) {
        ImGui::CloseCurrentPopup();
    }
    end_styled_modal();
}

void draw_add_account(app::AppState& state) {

    ImGui::TextUnformatted("Add account");
    ImGui::Spacing();

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
        ImGuiTabItemFlags full_flags = ImGuiTabItemFlags_None;
        if (force_full_login) full_flags |= ImGuiTabItemFlags_SetSelected;
        if (ImGui::BeginTabItem("Full login", nullptr, full_flags)) {
            add_account_detail::draw_full_login(state);
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
        if (ImGui::BeginTabItem("Import cached")) {
            add_account_detail::draw_import_cached(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Import bundle")) {
            draw_import_bundle(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

}  // namespace sam::ui::screens
