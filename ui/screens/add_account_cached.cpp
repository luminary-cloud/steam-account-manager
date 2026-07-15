#include "ui/screens/add_account_detail.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/account_store/store.hpp"
#include "core/crypto/secure_string.hpp"
#include "core/steam_local/connect_cache.hpp"
#include "core/steam_local/loginusers.hpp"
#include "core/steam_login/session.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"

namespace sam::ui::screens {

namespace add_account_detail {

namespace {

// A logged-in account on this PC that has a usable cached login token and isn't
// already in the vault.
struct CachedCandidate {
    steam_local::LocalAccount local;
    crypto::SecureString token;     // decrypted refresh-token JWT
    std::int64_t expires = 0;
    std::uint64_t steam_id = 0;
    bool expired = false;
    bool selected = false;
};

void rescan(app::AppState& state, std::vector<CachedCandidate>& out) {
    out.clear();
    const std::int64_t now = now_seconds();
    for (const auto& la : steam_local::read_loginusers()) {
        if (la.account_name.empty()) continue;
        // Hide accounts already present in the vault.
        if (core::store::find_existing_account(state.vault, la.steam_id_64,
                                               la.account_name) != nullptr) {
            continue;
        }
        // Only importable ones: must have a decryptable cached login token.
        auto token = steam_local::read_connect_cache_token(la.account_name);
        if (!token) continue;
        const std::int64_t exp = steam_login::jwt_expiry(*token);
        if (exp == 0) continue;  // not a parseable JWT

        CachedCandidate c;
        c.local = la;
        c.expires = exp;
        c.expired = exp <= now;
        c.steam_id = steam_login::jwt_steam_id(*token);
        if (c.steam_id == 0) c.steam_id = la.steam_id_64;
        c.token = std::move(*token);
        c.selected = !c.expired;  // pre-tick the ones that will work right away
        out.push_back(std::move(c));
    }
}

// ImGui::TextWrapped wraps at the full work rect, but separator_text insets the
// right edge by kContentPaddingX. Push this so wrapped body text lines up with the
// separators (and PushItemWidth(-kContentPaddingX) widgets) instead of running to
// the window edge. Use TextUnformatted/Text after this, not TextWrapped (which
// overrides the wrap pos).
void push_inset_wrap() {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x - kContentPaddingX);
}

}  // namespace

void draw_import_cached(app::AppState& state) {
    static std::vector<CachedCandidate> candidates;
    static bool scanned = false;
    static int imported_count = 0;
    static int expired_count = 0;
    static bool has_summary = false;

    if (!scanned) {
        rescan(state, candidates);
        scanned = true;
    }

    separator_text("Import from this PC");
    ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
    push_inset_wrap();
    ImGui::TextUnformatted(
        "Steam accounts signed in on this PC with their login remembered. Pick the "
        "ones to add; each is imported as a token-only account into the Cached group "
        "(tagged Cached), so Login signs in via token injection. You can later promote "
        "one to full access from its Edit dialog. Accounts already in your vault, and "
        "ones without a stored login token, are not listed.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (action_button("Rescan this PC")) {
        rescan(state, candidates);
        has_summary = false;
    }

    if (candidates.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No new cached accounts with a stored login token found.");
        if (has_summary) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
            ImGui::Text("Imported %d account(s).", imported_count);
            ImGui::PopStyleColor();
        }
        return;
    }

    ImGui::SameLine();
    if (action_button("Select all")) {
        for (auto& c : candidates) c.selected = true;
    }
    ImGui::SameLine();
    if (action_button("Select none")) {
        for (auto& c : candidates) c.selected = false;
    }

    ImGui::Spacing();
    const std::int64_t now = now_seconds();
    int selected = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto& c = candidates[i];
        if (c.selected) ++selected;

        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox("##sel", &c.selected);
        ImGui::SameLine();

        const std::string& persona = c.local.persona_name;
        const std::string label = persona.empty()
            ? c.local.account_name
            : persona + "  (" + c.local.account_name + ")";
        ImGui::TextUnformatted(label.c_str());

        if (c.steam_id != 0) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::Text("· %llu", static_cast<unsigned long long>(c.steam_id));
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        if (c.expired) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
            ImGui::TextUnformatted("· token expired");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::Text("· valid %lld day(s)",
                        static_cast<long long>((c.expires - now) / 86400));
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }

    if (state.settings.web_api_key.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        push_inset_wrap();
        ImGui::TextUnformatted("No Steam Web API key set. Stats (level, games, bans) won't "
                               "populate until you add one in Settings.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(selected == 0);
    if (action_button("Import selected")) {
        imported_count = 0;
        expired_count = 0;
        // Reserved ids stay valid even if the vaults' vectors reallocate during import.
        const std::string cached_group = core::store::ensure_cached_group(state.vault);
        const std::string cached_tag = core::store::ensure_cached_tag(state.vault);
        std::vector<std::string> account_ids;
        for (const auto& c : candidates) {
            if (!c.selected) continue;
            const std::string raw =
                c.local.account_name + "----" + std::string(c.token.begin(), c.token.end());
            const JwtImportResult r = import_jwt_token(state, raw, /*assign_nfa_group=*/false);
            if (!r.ok) continue;
            ++imported_count;
            if (r.expired) ++expired_count;
            state.nfa_dead_notified.erase(r.account_id);
            // Override import_jwt_token's NFA group with the Cached group + tag so these
            // are distinguishable from plain NFA-token accounts.
            if (core::Account* a = state.find_account(r.account_id)) {
                a->group_id = cached_group;
                if (std::find(a->tag_ids.begin(), a->tag_ids.end(), cached_tag) ==
                    a->tag_ids.end()) {
                    a->tag_ids.push_back(cached_tag);
                }
            }
            account_ids.push_back(r.account_id);
        }
        if (!account_ids.empty()) {
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            if (account_ids.size() == 1) {
                state.pull_all_for_account(account_ids.front());
            } else {
                state.refresh_accounts_staggered(std::move(account_ids));
                state.start_gc_validate(/*force=*/false);  // cached = NFA: validate + pull GC
            }
        }
        has_summary = true;
        // Imported accounts are now in the vault; drop them from the list.
        rescan(state, candidates);
    }
    ImGui::EndDisabled();

    if (has_summary) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::success());
        ImGui::Text("Imported %d account(s).", imported_count);
        ImGui::PopStyleColor();
        if (expired_count > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            push_inset_wrap();
            ImGui::Text("%d had an expired token: added, but Login won't work "
                        "until you sign that account into Steam again (its token "
                        "refreshes) and re-import, or add a password.",
                        expired_count);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }
}

}  // namespace add_account_detail

}  // namespace sam::ui::screens
