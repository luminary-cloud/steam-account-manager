#include "ui/widgets/login_method_control.hpp"

#include "app/gamesense_loader.hpp"
#include "app/luminary_loader.hpp"
#include "ui/util.hpp"

namespace sam::ui::widgets {

namespace {

const char* method_chip_label(core::LoginMethod m) {
    switch (m) {
        case core::LoginMethod::LaunchCs2:          return "CS2";
        case core::LoginMethod::LaunchCs2Gamesense: return "CS2+GS";
        case core::LoginMethod::LaunchCs2Luminary:  return "CS2+LUM";
        case core::LoginMethod::Normal:             break;
    }
    return "";
}

const char* method_tooltip(core::LoginMethod m) {
    switch (m) {
        case core::LoginMethod::LaunchCs2:
            return "On login: sign in, then launch CS2.";
        case core::LoginMethod::LaunchCs2Gamesense:
            return "On login: sign in, launch CS2, then inject gamesense.";
        case core::LoginMethod::LaunchCs2Luminary:
            return "On login: sign in, launch CS2, then run luminary loader.";
        case core::LoginMethod::Normal:
            break;
    }
    return "On login: sign in only. Use the arrow to also launch CS2.";
}

void draw_sign_in_override(app::AppState& state, const core::Account& a) {
    const app::SignInMethod def = state.settings.sign_in_method;
    const app::SignInMethod eff = state.effective_sign_in_method(a.id);
    const bool overridden = state.sign_in_override.count(a.id) != 0;

    auto entry = [&](const char* label, app::SignInMethod m, bool capable,
                     const char* blocked_tip, const char* override_tip) {

        const bool is_default = (m == def);
        const bool disabled = (is_default && !overridden) || !capable;

        ImGui::BeginDisabled(disabled);
        if (ImGui::Selectable(label, eff == m)) {
            if (is_default) state.sign_in_override.erase(a.id);
            else            state.sign_in_override[a.id] = m;
        }
        ImGui::EndDisabled();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!capable) {
                set_tooltip("%s", blocked_tip);
            } else if (is_default && !overridden) {
                set_tooltip("Already the default, set in Settings > Steam login.");
            } else if (is_default) {
                set_tooltip("Drop the override and follow Settings again.");
            } else {
                set_tooltip("%s", override_tip);
            }
        }
    };

    const bool has_password = !a.password.empty();
    entry("Sign-in driver", app::SignInMethod::Driver, has_password,
          "Needs a stored password: the driver types it into Steam's login window.",
          "Sign this account in through the login window, until the app restarts.");
    entry("Token injection", app::SignInMethod::TokenInject,
          has_password || app::cm_token_valid(a),
          "Needs a stored login token, or a password to mint one from.",
          "Sign this account in without a login window, until the app restarts.");
}

}  // namespace

ImU32 login_method_color(core::LoginMethod m) {
    switch (m) {
        case core::LoginMethod::LaunchCs2:
            return ImGui::GetColorU32(ImVec4(0.20F, 0.45F, 0.85F, 1.0F));
        case core::LoginMethod::LaunchCs2Gamesense:
            return ImGui::GetColorU32(ImVec4(0.55F, 0.35F, 0.85F, 1.0F));
        case core::LoginMethod::LaunchCs2Luminary:
            return ImGui::GetColorU32(ImVec4(0.20F, 0.70F, 0.55F, 1.0F));
        case core::LoginMethod::Normal:
            break;
    }
    return ImGui::GetColorU32(ImGuiCol_Text);
}

void draw_login_method_chip(core::LoginMethod m) {
    if (m == core::LoginMethod::Normal) return;
    const char* label = method_chip_label(m);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float text_h = ImGui::GetTextLineHeight();
    const float pad_x = 5.0F;
    const float w = ts.x + pad_x * 2.0F;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + text_h), login_method_color(m),
                      text_h * 0.4F);
    dl->AddText(ImVec2(p.x + pad_x, p.y + (text_h - ts.y) * 0.5F),
                IM_COL32_WHITE, label);

    ImGui::Dummy(ImVec2(w, text_h));
    if (ImGui::IsItemHovered()) set_tooltip("%s", method_tooltip(m));
}

bool draw_login_split_button(app::AppState& state, core::Account& a, float total_width) {
    constexpr float kHeight   = 26.0F;
    constexpr float kRounding = 6.0F;
    const float caret_w = kLoginCaretWidth;
    const float login_w = total_width - caret_w;

    const ImVec2 origin = ImGui::GetCursorScreenPos();

    const bool blocked_by_clean = state.cleaner_busy.load();

    ImGui::BeginDisabled(blocked_by_clean);
    const bool login_clicked = ImGui::InvisibleButton("##login-half", ImVec2(login_w, kHeight));
    ImGui::EndDisabled();
    const bool login_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool login_active  = ImGui::IsItemActive();
    ImGui::SameLine(0.0F, 0.0F);
    const bool caret_clicked = ImGui::InvisibleButton("##caret-half", ImVec2(caret_w, kHeight));
    const bool caret_hovered = ImGui::IsItemHovered();
    const bool caret_active  = ImGui::IsItemActive();

    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 br(origin.x + total_width, origin.y + kHeight);
    const float seam_x = origin.x + login_w;

    dl->AddRectFilled(origin, br, ImGui::GetColorU32(ImGuiCol_Button), kRounding);
    if (!blocked_by_clean && (login_hovered || login_active)) {
        dl->AddRectFilled(origin, ImVec2(seam_x, br.y),
                          ImGui::GetColorU32(login_active ? ImGuiCol_ButtonActive
                                                          : ImGuiCol_ButtonHovered),
                          kRounding, ImDrawFlags_RoundCornersLeft);
    }
    if (caret_hovered || caret_active) {
        dl->AddRectFilled(ImVec2(seam_x, origin.y), br,
                          ImGui::GetColorU32(caret_active ? ImGuiCol_ButtonActive
                                                          : ImGuiCol_ButtonHovered),
                          kRounding, ImDrawFlags_RoundCornersRight);
    }
    dl->AddLine(ImVec2(seam_x, origin.y + 4.0F), ImVec2(seam_x, br.y - 4.0F),
                ImGui::GetColorU32(ImGuiCol_Separator), 1.0F);

    const char* label = "Login";
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(origin.x + (login_w - ts.x) * 0.5F,
                       origin.y + (kHeight - ts.y) * 0.5F),
                ImGui::GetColorU32(blocked_by_clean ? ImGuiCol_TextDisabled : ImGuiCol_Text),
                label);

    const core::LoginMethod eff_method =
        core::effective_login_method(a.login_method, state.settings.safe_mode);

    const float cx = seam_x + caret_w * 0.5F;
    const float cy = origin.y + kHeight * 0.5F;
    const float r = 3.5F;
    dl->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.5F), ImVec2(cx + r, cy - r * 0.5F),
                          ImVec2(cx, cy + r), login_method_color(eff_method));

    if (login_hovered) {
        if (blocked_by_clean) {
            set_tooltip("The cleaner is running. Launching now would be undone by it.");
        } else {
            set_tooltip("%s", method_tooltip(eff_method));
        }
    }
    if (caret_hovered) {
        set_tooltip(state.settings.safe_mode
            ? "Set what Login does, and override the sign-in method for this session."
            : "Set what Login does (CS2, loaders), and override the sign-in method "
              "for this session.");
    }

    if (caret_clicked) ImGui::OpenPopup("##login-method-popup");

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    if (ImGui::BeginPopup("##login-method-popup")) {
        auto set_method = [&](core::LoginMethod m) {
            a.login_method = m;
            state.vault_dirty = true;
            state.save_vault_if_dirty();
        };
        if (ImGui::Selectable("Normal login",
                              eff_method == core::LoginMethod::Normal)) {
            set_method(core::LoginMethod::Normal);
        }
        if (ImGui::Selectable("Launch CS2",
                              eff_method == core::LoginMethod::LaunchCs2)) {
            set_method(core::LoginMethod::LaunchCs2);
        }

        if (!state.settings.safe_mode) {
            if (ImGui::Selectable("Launch CS2 + gamesense",
                                  a.login_method == core::LoginMethod::LaunchCs2Gamesense)) {
                if (app::gamesense_loader_path()) {
                    set_method(core::LoginMethod::LaunchCs2Gamesense);
                } else {
                    state.gamesense_pick_request = a.id;
                }
            }
            if (ImGui::Selectable("Launch CS2 + luminary",
                                  a.login_method == core::LoginMethod::LaunchCs2Luminary)) {
                if (app::luminary_loader_path()) {
                    set_method(core::LoginMethod::LaunchCs2Luminary);
                } else {
                    state.luminary_pick_request = a.id;
                }
            }
        }

        if (!a.is_nfa) {
            ImGui::Separator();
            draw_sign_in_override(state, a);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    return login_clicked;
}

}  // namespace sam::ui::widgets
