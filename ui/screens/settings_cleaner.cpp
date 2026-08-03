#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/cleaner_runner.hpp"
#include "app/job_pump.hpp"
#include "core/cleaner/profiles.hpp"
#include "core/cleaner/steam_scan.hpp"
#include "ui/screens/settings_sections.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/redacted_text.hpp"

namespace sam::ui::screens {
namespace settings_detail {
namespace {

std::string format_bytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

const char* op_kind_label(cleaner::OpKind k) {
    switch (k) {
        case cleaner::OpKind::RemoveFile: return "FILE";
        case cleaner::OpKind::RemoveTree: return "DIR";
        case cleaner::OpKind::RemoveRegistryValue: return "REGV";
        case cleaner::OpKind::RemoveRegistryKey: return "REGK";
        case cleaner::OpKind::WriteRegistryString: return "WRITE";
        case cleaner::OpKind::VdfRemoveChild: return "VDF";
        case cleaner::OpKind::VdfSetValue: return "VDF-SET";
    }
    return "?";
}

struct PreserveRow {
    std::uint64_t steam_id = 0;
    std::string primary;
    std::string secondary;
    bool in_vault = false;
};

struct DiskScan {
    bool loaded = false;
    std::vector<cleaner::AccountInfo> accounts;
};

DiskScan& disk_scan() {
    static DiskScan s;
    return s;
}

void refresh_disk_scan() {
    auto& scan = disk_scan();
    scan.accounts.clear();
    if (auto install = cleaner::discover_install()) {
        scan.accounts = cleaner::enumerate_accounts(*install);
    }
    scan.loaded = true;
}

std::vector<PreserveRow> build_rows(const app::AppState& state) {
    std::vector<PreserveRow> rows;
    rows.reserve(state.vault.accounts.size() + disk_scan().accounts.size());

    for (const auto& a : state.vault.accounts) {
        PreserveRow r;
        r.steam_id = a.steam_id_64;
        r.primary = !a.display_name.empty() ? to_utf8(a.display_name) : a.web.persona_name;
        r.secondary = widgets::login_label(state, a);
        r.in_vault = true;
        rows.push_back(std::move(r));
    }

    for (const auto& acc : disk_scan().accounts) {

        const std::string digits = to_utf8(acc.steamid64);
        std::uint64_t sid = 0;
        const auto [_, ec] =
            std::from_chars(digits.data(), digits.data() + digits.size(), sid);
        if (ec != std::errc{} || sid == 0) continue;
        const bool known = std::any_of(rows.begin(), rows.end(),
                                       [&](const PreserveRow& r) { return r.steam_id == sid; });
        if (known) continue;
        PreserveRow r;
        r.steam_id = sid;
        r.primary = to_utf8(acc.persona_name);
        r.secondary = to_utf8(acc.account_name);
        r.in_vault = false;
        rows.push_back(std::move(r));
    }
    return rows;
}

bool is_preserved(const app::Settings& s, std::uint64_t steam_id) {
    const auto& ids = s.cleaner.preserved_steam_ids;
    return std::find(ids.begin(), ids.end(), steam_id) != ids.end();
}

void set_preserved(app::Settings& s, std::uint64_t steam_id, bool on) {
    auto& ids = s.cleaner.preserved_steam_ids;
    const auto it = std::find(ids.begin(), ids.end(), steam_id);
    if (on && it == ids.end()) {
        ids.push_back(steam_id);
    } else if (!on && it != ids.end()) {
        ids.erase(it);
    }
}

void draw_keep_list(app::AppState& state) {
    separator_text("Accounts to keep");

    if (!disk_scan().loaded) refresh_disk_scan();

    const bool touches_accounts =
        state.settings.cleaner.profile != app::CleanerProfile::QuickClean;
    if (!touches_accounts) {
        ImGui::TextDisabled("Quick Clean never touches account data, so this list has no effect "
                            "on it.");
        ImGui::Spacing();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
    ImGui::TextWrapped("Only the accounts ticked here survive a clean. Every other Steam account "
                       "on this PC is signed out and loses its saved login - including the one "
                       "you are about to launch and the one currently signed in.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const auto rows = build_rows(state);
    if (rows.empty()) {
        ImGui::TextDisabled("No accounts found in this vault or on this PC.");
    } else {
        constexpr float kKeepColW = 44.0F;

        const float cell_pad_y = ImGui::GetStyle().CellPadding.y;
        const float row_h  = ImGui::GetFrameHeight() + cell_pad_y * 2.0F;
        const float head_h = ImGui::GetTextLineHeight() + cell_pad_y * 2.0F;

        const float table_h =
            head_h + row_h * static_cast<float>(std::min(rows.size(), std::size_t{8})) + 4.0F;

        const float table_w =
            std::min(ImGui::GetContentRegionAvail().x - kContentPaddingX, 620.0F);

        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0F, 1.0F, 1.0F, 0.025F));
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingStretchProp |
                                           ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##cleaner-keep", 4, kFlags, ImVec2(table_w, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("##keep",  ImGuiTableColumnFlags_WidthFixed, kKeepColW);
            ImGui::TableSetupColumn("##acct",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##login", ImGuiTableColumnFlags_WidthFixed, 180.0F);
            ImGui::TableSetupColumn("##sid",   ImGuiTableColumnFlags_WidthFixed, 160.0F);

            ImGui::TableNextRow();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::dim_text());
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Keep");
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Account");
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Login");
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Steam ID");
            ImGui::PopStyleColor();

            for (std::size_t i = 0; i < rows.size(); ++i) {
                const PreserveRow& r = rows[i];
                ImGui::TableNextRow();

                ImGui::PushID(static_cast<int>(i));

                ImGui::TableNextColumn();
                const float box = ImGui::GetFrameHeight();
                const float avail = ImGui::GetContentRegionAvail().x;
                if (avail > box)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - box) * 0.5F);
                if (r.steam_id == 0) {

                    ImGui::BeginDisabled(true);
                    bool off = false;
                    ImGui::Checkbox("##keep", &off);
                    ImGui::EndDisabled();

                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        set_tooltip("No Steam ID yet, so a clean will wipe this account. "
                                    "Refresh it first to be able to keep it.");
                    }
                } else {
                    bool keep = is_preserved(state.settings, r.steam_id);
                    if (ImGui::Checkbox("##keep", &keep)) {
                        set_preserved(state.settings, r.steam_id, keep);
                        state.save_settings();
                    }
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.primary.empty() ? "(no name)" : r.primary.c_str());
                if (!r.in_vault) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(not in vault)");
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.secondary.c_str());

                ImGui::TableNextColumn();
                if (r.steam_id == 0) {
                    ImGui::TextDisabled("none yet");
                } else {
                    ImGui::Text("%llu", static_cast<unsigned long long>(r.steam_id));
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleColor();
    }

    if (action_button("Rescan this PC", ImVec2(150, 0))) refresh_disk_scan();
    hover_tooltip("Re-reads Steam's logins, so accounts added since this screen opened "
                  "show up.");
}

void draw_preview_and_run(app::AppState& state) {
    separator_text("Preview and run");

    static bool previewing = false;
    static std::string preview_error;
    const bool busy = previewing || state.cleaner_busy.load();

    ImGui::BeginDisabled(busy);
    if (action_button("Preview", ImVec2(120, 0))) {
        previewing = true;
        preview_error.clear();
        const app::Settings snapshot = state.settings;
        app::job_pump::submit([&state, snapshot] {
            auto plan = app::cleaner_runner::build(snapshot, true);
            state.post_ui_callback([&state, plan = std::move(plan)] {
                previewing = false;
                if (!plan) preview_error = "Steam install not found on this PC.";
                state.cleaner_preview = plan;
            });
        });
    }
    ImGui::EndDisabled();
    hover_tooltip("Lists exactly what this profile would touch, with sizes. "
                  "Nothing is deleted.");

    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (action_button("Run now", ImVec2(120, 0))) ImGui::OpenPopup("Run the cleaner?");
    ImGui::EndDisabled();
    hover_tooltip("Closes Steam, then deletes. There is no backup and no undo.");

    if (busy) {
        ImGui::SameLine();
        ImGui::TextDisabled(previewing ? "Building preview..." : "Cleaning...");
    } else if (!preview_error.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextUnformatted(preview_error.c_str());
        ImGui::PopStyleColor();
    }

    if (state.cleaner_preview) {
        const auto& plan = *state.cleaner_preview;
        ImGui::Spacing();
        ImGui::Text("Plan: %zu steps, %s across %llu files", plan.steps.size(),
                    format_bytes(plan.total_bytes).c_str(),
                    static_cast<unsigned long long>(plan.total_file_count));

        const float list_w = ImGui::GetContentRegionAvail().x - kContentPaddingX;
        if (ImGui::BeginChild("##cleaner-plan", ImVec2(list_w, 190.0F),
                              ImGuiChildFlags_Borders)) {
            for (const auto& step : plan.steps) {
                std::string row = std::string{"["} + op_kind_label(step.op.kind) + "] " +
                                  to_utf8(step.op.target);
                if (!step.op.value_name.empty()) row += " :: " + to_utf8(step.op.value_name);
                if (!step.op.payload.empty()) row += " = \"" + to_utf8(step.op.payload) + "\"";
                ImGui::TextWrapped("%s", row.c_str());
            }
        }
        ImGui::EndChild();
    }

    if (state.cleaner_last) {
        const auto& r = *state.cleaner_last;
        ImGui::Spacing();
        ImGui::Text("Last run: %d removed, %d failed", r.succeeded, r.failed);
        if (!r.failure_messages.empty() && ImGui::CollapsingHeader("Failures")) {
            ImGui::Indent();
            for (const auto& m : r.failure_messages) {
                ImGui::TextWrapped("%s", to_utf8(m).c_str());
            }
            ImGui::Unindent();
        }
    }
}

}  // namespace

void draw_cleaner_section(app::AppState& state) {
    const auto profiles = cleaner::built_in_profiles();
    auto& cfg = state.settings.cleaner;

    separator_text("Steam tracer cleaner");
    ImGui::TextWrapped("Deletes what Steam leaves on this PC: caches and logs, and - depending "
                       "on the profile - saved logins, login tokens and per-account data.");
    ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
    ImGui::TextWrapped("Nothing is backed up and a clean cannot be undone. Preview before you "
                       "arm a trigger.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (ImGui::Checkbox("Enable cleaner", &cfg.enabled)) state.save_settings();
    hover_tooltip("Master switch for the triggers below and Run now. Safe mode also "
                  "holds all of them off.");

    ImGui::BeginDisabled(!cfg.enabled);

    ImGui::Spacing();
    separator_text("Profile");
    {
        int idx = static_cast<int>(cfg.profile);
        ImGui::SetNextItemWidth(220);
        if (styled_combo("Cleanup profile", &idx, "Quick Clean\0Account Reset\0Full Wipe\0")) {
            const auto picked = static_cast<app::CleanerProfile>(idx);
            const auto slot = static_cast<std::size_t>(idx);

            if (slot < profiles.size() && profiles[slot].requires_confirmation) {
                ImGui::OpenPopup("Use the Full Wipe profile?");
            } else {
                cfg.profile = picked;
                state.cleaner_preview.reset();
                state.save_settings();
            }
        }
        const auto slot = static_cast<std::size_t>(cfg.profile);
        if (slot < profiles.size()) {
            ImGui::TextWrapped("%s", to_utf8(profiles[slot].description).c_str());
        }
    }

    ImGui::Spacing();
    separator_text("Run automatically");
    if (ImGui::Checkbox("Before signing an account in", &cfg.run_before_launch))
        state.save_settings();
    hover_tooltip("Cleans while the launch already has Steam shut down. Costs a few "
                  "seconds and never fights the sign-in, so this is the safe trigger.");

    if (ImGui::Checkbox("When the vault is unlocked", &cfg.run_on_unlock)) state.save_settings();
    hover_tooltip("Runs once per vault open. Closes Steam, signing out anyone not on "
                  "the keep list.");

    if (ImGui::Checkbox("When the app closes", &cfg.run_on_exit)) state.save_settings();
    hover_tooltip("Leaves the PC clean on exit. Closes Steam, ending any open game.");

    ImGui::Spacing();
    draw_keep_list(state);

    ImGui::Spacing();
    draw_preview_and_run(state);

    ImGui::EndDisabled();

    if (begin_styled_modal("Use the Full Wipe profile?")) {
        ImGui::TextWrapped("Full Wipe also deletes each non-preserved account's whole userdata "
                           "folder: cloud saves, game settings, CS2 configs and launch options, "
                           "controller bindings, screenshots and non-Steam shortcuts. None of it "
                           "is backed up.");
        ImGui::Spacing();
        if (action_button("Cancel", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (action_button("Use Full Wipe", ImVec2(150, 0))) {
            cfg.profile = app::CleanerProfile::FullWipe;
            state.cleaner_preview.reset();
            state.save_settings();
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }

    if (begin_styled_modal("Run the cleaner?")) {
        const auto slot = static_cast<std::size_t>(cfg.profile);
        const std::string name =
            slot < profiles.size() ? to_utf8(profiles[slot].name) : std::string{"?"};
        ImGui::TextWrapped("This closes Steam and runs the %s profile now. Deletions are "
                           "immediate and cannot be undone.", name.c_str());
        ImGui::Spacing();
        if (action_button("Cancel", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (action_button("Run", ImVec2(110, 0))) {
            app::cleaner_runner::run_async(state, app::cleaner_runner::Trigger::Manual);
            ImGui::CloseCurrentPopup();
        }
        end_styled_modal();
    }
}

}  // namespace settings_detail
}  // namespace sam::ui::screens
