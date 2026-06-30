#include "ui/screens/confirmations_detail.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/job_pump.hpp"
#include "core/log.hpp"
#include "core/sda/confirmation.hpp"
#include "core/sda/mafile.hpp"
#include "core/strings.hpp"
#include "platform/file_dialog.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/redacted_text.hpp"
#include "ui/widgets/search_bar.hpp"

namespace sam::ui::screens {

namespace confirmations_detail {

bool needs_mafile_data(const core::Account& a) {
    if (!a.sda.has_value()) return false;
    return a.sda->identity_secret.empty() || a.sda->device_id.empty();
}

namespace {

void merge_guard_into_account(core::Account& a, const core::SteamGuardAccount& g) {
    if (!a.sda.has_value()) a.sda = core::SteamGuardAccount{};
    auto& s = *a.sda;
    if (!g.shared_secret.empty())   s.shared_secret   = g.shared_secret;
    if (!g.serial_number.empty())   s.serial_number   = g.serial_number;
    if (!g.revocation_code.empty()) s.revocation_code = g.revocation_code;
    if (!g.uri.empty())             s.uri             = g.uri;
    if (g.server_time != 0)         s.server_time     = g.server_time;
    if (s.account_name.empty())     s.account_name    = g.account_name;
    if (!g.token_gid.empty())       s.token_gid       = g.token_gid;
    if (!g.identity_secret.empty()) s.identity_secret = g.identity_secret;
    if (!g.secret_1.empty())        s.secret_1        = g.secret_1;
    if (!g.device_id.empty())       s.device_id       = g.device_id;
    s.status         = g.status;
    s.fully_enrolled = g.fully_enrolled;
}

bool mafile_matches_account(const sam::sda::MafileLoadResult& loaded,
                             const std::string& login_lower,
                             std::uint64_t steam_id_64) {
    if (loaded.session_steam_id != 0 && steam_id_64 != 0 &&
        loaded.session_steam_id == steam_id_64) {
        return true;
    }
    if (!loaded.guard.account_name.empty()) {
        return core::to_lower(loaded.guard.account_name) == login_lower;
    }
    return false;
}

}  // namespace

void link_mafile_for_account(app::AppState& state, const core::Account& a) {
    platform::file_dialog::Options opts;
    opts.parent = state.main_hwnd;
    opts.title = L"Choose maFile for this account";
    opts.filters = {
        {L"Steam mobile authenticator (*.maFile)", L"*.maFile"},
        {L"All files (*.*)", L"*.*"},
    };
    const auto picked = platform::file_dialog::open_file(opts);
    if (!picked.ok) return;

    const std::string aid = a.id;
    const std::string login_lower = core::to_lower(a.login);
    const std::uint64_t sid = a.steam_id_64;
    const std::filesystem::path path = picked.path;

    {
        std::lock_guard lk(g_mtx);
        g_lists[aid].refreshing = true;
        g_lists[aid].last_error.clear();
    }

    app::job_pump::submit([&state, aid, login_lower, sid, path]() mutable {
        sam::sda::MafileLoadResult loaded;
        try {
            loaded = sam::sda::load_mafile(path, {});
        } catch (const sam::sda::MafileEncrypted&) {
            std::lock_guard lk(g_mtx);
            g_lists[aid].refreshing = false;
            g_lists[aid].last_error = "maFile is encrypted; decrypt it first";
            return;
        } catch (const std::exception& ex) {
            std::lock_guard lk(g_mtx);
            g_lists[aid].refreshing = false;
            g_lists[aid].last_error = std::string{"maFile load failed: "} + ex.what();
            return;
        }

        if (!mafile_matches_account(loaded, login_lower, sid)) {
            std::lock_guard lk(g_mtx);
            g_lists[aid].refreshing = false;
            g_lists[aid].last_error =
                "maFile does not match this account (got '" +
                loaded.guard.account_name + "')";
            return;
        }

        state.post_ui_callback([&state, aid, guard = std::move(loaded.guard)]() mutable {
            auto* acc = state.find_account(aid);
            if (!acc) return;
            merge_guard_into_account(*acc, guard);
            state.vault_dirty = true;
            state.save_vault_if_dirty();
            SAM_LOG_INFO("confirmation: linked maFile for '{}'", acc->login);

            core::Account snap = *acc;
            {
                std::lock_guard lk(g_mtx);
                g_lists[aid].refreshing = false;
                g_lists[aid].last_error.clear();
            }
            submit_account_refresh(state, snap);
        });
    });
}

void scan_folder_for_mafiles(app::AppState& state) {
    if (g_scan_in_progress.load()) return;

    platform::file_dialog::Options opts;
    opts.parent = state.main_hwnd;
    opts.title = L"Pick folder containing maFiles";
    const auto picked = platform::file_dialog::pick_folder(opts);
    if (!picked.ok) return;

    struct Target {
        std::string id;
        std::string login_lower;
        std::string steam_id_str;
        std::uint64_t steam_id_64 = 0;
    };
    std::vector<Target> targets;
    for (const auto& a : state.vault.accounts) {
        if (!needs_mafile_data(a)) continue;
        Target t;
        t.id = a.id;
        t.login_lower = core::to_lower(a.login);
        t.steam_id_64 = a.steam_id_64;
        if (t.steam_id_64 != 0) t.steam_id_str = std::to_string(t.steam_id_64);
        targets.push_back(std::move(t));
    }

    if (targets.empty()) {
        std::lock_guard lk(g_mtx);
        g_last_scan = ScanResult{};
        g_show_scan_modal = true;
        return;
    }

    g_scan_in_progress.store(true);
    const int targeted_count = static_cast<int>(targets.size());
    const std::filesystem::path folder = picked.path;
    SAM_LOG_INFO("confirmation: scanning '{}' for {} missing accounts",
                 folder.string(), targeted_count);

    app::job_pump::submit([&state, folder, targets = std::move(targets),
                            targeted_count]() mutable {
        ScanResult res;
        res.targeted = targeted_count;

        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            folder, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator end;

        for (; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) { ec.clear(); continue; }
            const auto& path = it->path();
            const std::string ext = core::to_lower(path.extension().string());
            if (ext != ".mafile") continue;

            const std::string stem = core::to_lower(path.stem().string());

            int candidate = -1;
            int matches = 0;
            for (std::size_t i = 0; i < targets.size(); ++i) {
                if (!targets[i].steam_id_str.empty() &&
                    stem.find(targets[i].steam_id_str) != std::string::npos) {
                    candidate = static_cast<int>(i);
                    ++matches;
                }
            }
            if (matches == 0) {
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    if (targets[i].login_lower.empty()) continue;
                    if (stem.find(targets[i].login_lower) != std::string::npos) {
                        candidate = static_cast<int>(i);
                        ++matches;
                    }
                }
            }
            if (matches == 0) continue;
            if (matches > 1) {
                ++res.skipped_ambiguous;
                continue;
            }

            sam::sda::MafileLoadResult loaded;
            try {
                loaded = sam::sda::load_mafile(path, {});
            } catch (const sam::sda::MafileEncrypted&) {
                ++res.skipped_encrypted;
                continue;
            } catch (...) {
                ++res.parse_errors;
                continue;
            }

            const auto& tgt = targets[candidate];
            if (!mafile_matches_account(loaded, tgt.login_lower, tgt.steam_id_64)) {
                ++res.skipped_no_match;
                continue;
            }
            if (loaded.guard.identity_secret.empty() ||
                loaded.guard.device_id.empty()) {
                ++res.skipped_no_match;
                continue;
            }

            const std::string aid = tgt.id;
            state.post_ui_callback(
                [&state, aid, guard = std::move(loaded.guard)]() mutable {
                    auto* acc = state.find_account(aid);
                    if (!acc) return;
                    merge_guard_into_account(*acc, guard);
                    state.vault_dirty = true;
                    std::lock_guard lk(g_mtx);
                    if (g_lists[aid].last_error.find("missing maFile data") !=
                        std::string::npos) {
                        g_lists[aid].last_error.clear();
                    }
                });
            ++res.linked;
            targets.erase(targets.begin() + candidate);
            if (targets.empty()) break;
        }

        state.post_ui_callback([&state, res] {
            state.save_vault_if_dirty();
            std::lock_guard lk(g_mtx);
            g_last_scan = res;
            g_show_scan_modal = true;
            SAM_LOG_INFO("confirmation: scan done, linked {}/{} (skipped: "
                         "encrypted={} no_match={} ambiguous={} parse_errors={})",
                         res.linked, res.targeted,
                         res.skipped_encrypted, res.skipped_no_match,
                         res.skipped_ambiguous, res.parse_errors);
        });
        g_scan_in_progress.store(false);
    });
}

namespace {

struct TypeStyle {
    const char* label;
    ImVec4 fill;
};

TypeStyle style_for(sda::ConfirmationType t) {
    switch (t) {
        case sda::ConfirmationType::Trade:             return {"Trade",    theme::accent()};
        case sda::ConfirmationType::MarketListing:     return {"Market",   theme::accent()};
        case sda::ConfirmationType::PhoneNumberChange: return {"Phone",    theme::warning()};
        case sda::ConfirmationType::AccountRecovery:   return {"Recovery", theme::danger()};
        case sda::ConfirmationType::FeatureOptOut:     return {"Opt-out",  theme::warning()};
        case sda::ConfirmationType::Unknown:
        default:                                       return {"Other",    theme::dim_text()};
    }
}

std::string format_relative(std::int64_t unix_seconds) {
    if (unix_seconds <= 0) return {};
    const auto delta = now_seconds() - unix_seconds;
    if (delta < 0)          return "in the future";
    if (delta < 60)         return std::to_string(delta) + "s ago";
    if (delta < 3600)       return std::to_string(delta / 60) + "m ago";
    if (delta < 86400)      return std::to_string(delta / 3600) + "h ago";
    if (delta < 86400 * 30) return std::to_string(delta / 86400) + "d ago";

    const auto t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

void draw_icon(const sda::Confirmation& c, float size) {
    const float rounding = 6.0F;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(theme::panel_hover());
    dl->AddRectFilled(cursor, ImVec2(cursor.x + size, cursor.y + size), bg, rounding);
    if (c.icon_url.empty()) return;
    if (auto* srv = widgets::avatar_for(c.icon_url)) {
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv),
                            cursor, ImVec2(cursor.x + size, cursor.y + size),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, rounding);
    }
}

}  // namespace

CardAction draw_confirmation_card(const sda::Confirmation& c, float width,
                                   bool* selected) {
    CardAction action = CardAction::None;

    ImGui::PushID(c.id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::panel());
    ImGui::PushStyleColor(ImGuiCol_Border,
                          (selected && *selected) ? theme::accent() : theme::border());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));

    ImGui::BeginChild("##conf-card", ImVec2(width, kCardHeight),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    const float row_top  = ImGui::GetCursorPosY();
    const float row_left = ImGui::GetCursorPosX();

    if (selected) {
        ImGui::Checkbox("##sel", selected);
        ImGui::SameLine(0.0F, 8.0F);
    }
    draw_icon(c, kIconSize);
    ImGui::SameLine(0.0F, 12.0F);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(c.headline.c_str());
    const std::string rel = format_relative(c.creation_unix);
    if (!rel.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextUnformatted(rel.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    {
        const auto style = style_for(c.type);
        const float right_pad = 6.0F;
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowSize().x - kPillWidth - right_pad,
                                   row_top + 2.0F));
        draw_pill(style.label, style.fill, true, kPillWidth);
    }

    ImGui::SetCursorPos(ImVec2(row_left,
                               row_top + kIconSize + ImGui::GetStyle().ItemSpacing.y));

    if (!c.summary.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TextWrapped("%s", c.summary.c_str());
        ImGui::PopStyleColor();
    }

    const float spacing   = ImGui::GetStyle().ItemSpacing.x;
    const float content_w = width - 2.0F * ImGui::GetStyle().WindowPadding.x;
    const float btn_w     = (content_w - spacing) * 0.5F;
    const float btn_y     = ImGui::GetWindowSize().y - kButtonRowH;
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, btn_y));

    if (action_button("Allow", ImVec2(btn_w, 0))) action = CardAction::Allow;
    ImGui::SameLine();
    if (action_button("Deny",  ImVec2(btn_w, 0))) action = CardAction::Deny;

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::PopID();

    return action;
}

void draw_scan_result_modal() {
    if (g_show_scan_modal) {
        ImGui::OpenPopup("Scan maFiles");
        g_show_scan_modal = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    if (!ImGui::BeginPopupModal("Scan maFiles", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PopStyleVar();
        return;
    }

    ScanResult r;
    {
        std::lock_guard lk(g_mtx);
        r = g_last_scan;
    }

    if (r.targeted == 0) {
        ImGui::TextUnformatted("No accounts are missing maFile data.");
    } else {
        ImGui::Text("Linked %d of %d missing accounts.", r.linked, r.targeted);
        if (r.skipped_encrypted || r.skipped_no_match ||
            r.skipped_ambiguous || r.parse_errors) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::TextUnformatted("Skipped:");
            if (r.skipped_encrypted)
                ImGui::BulletText("encrypted: %d", r.skipped_encrypted);
            if (r.skipped_ambiguous)
                ImGui::BulletText("ambiguous filename: %d", r.skipped_ambiguous);
            if (r.skipped_no_match)
                ImGui::BulletText("content did not match: %d", r.skipped_no_match);
            if (r.parse_errors)
                ImGui::BulletText("parse errors: %d", r.parse_errors);
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    if (action_button("OK")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

void draw_history_modal(app::AppState& state, bool* p_open) {
    if (!*p_open) return;
    ImGui::OpenPopup("Confirmation history");
    ImGui::SetNextWindowSize(ImVec2(760.0F, 480.0F), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    if (!ImGui::BeginPopupModal("Confirmation history", p_open,
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar(2);
        return;
    }

    static std::string g_history_search;
    widgets::draw_search_bar(g_history_search, 320.0F);
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu entries)", state.conf_audit.entries().size());

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035F, 0.035F, 0.035F, 1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F);
    ImGui::BeginChild("##history-body", ImVec2(0, -36.0F));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0F, 1.0F, 1.0F, 0.025F));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##history", 5, flags)) {
        ImGui::TableSetupColumn("##time", ImGuiTableColumnFlags_WidthFixed, 110.0F);
        ImGui::TableSetupColumn("##account", ImGuiTableColumnFlags_WidthFixed, 140.0F);
        ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("##source", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableSetupColumn("##headline", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Time");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Account");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Action");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Source");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Headline");
        ImGui::PopStyleColor();

        std::string search_lower = g_history_search;
        for (char& c : search_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        const auto& es = state.conf_audit.entries();
        for (auto it = es.rbegin(); it != es.rend(); ++it) {
            const auto& e = *it;
            if (!search_lower.empty()) {
                auto contains = [&](const std::string& s) {
                    std::string lo = s;
                    for (char& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return lo.find(search_lower) != std::string::npos;
                };
                if (!contains(e.account_login) && !contains(e.headline)) continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char tbuf[24];
            std::time_t t = static_cast<std::time_t>(e.unix_time);
            std::tm tm{};
            localtime_s(&tm, &t);
            std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
            ImGui::TextUnformatted(tbuf);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.account_login.c_str());
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                   e.allow ? theme::success() : theme::danger());
            ImGui::TextUnformatted(e.allow ? "allow" : "deny");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            const char* src = "user";
            switch (e.source) {
                case sda::AuditSource::UserSingle: src = "user";   break;
                case sda::AuditSource::UserBulk:   src = "bulk";   break;
                case sda::AuditSource::Auto:       src = "auto";   break;
            }
            ImGui::TextDisabled("%s", src);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", e.headline.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor();
    ImGui::EndChild();

    if (action_button("Close", ImVec2(100, 0))) {
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

}  // namespace confirmations_detail

}  // namespace sam::ui::screens
