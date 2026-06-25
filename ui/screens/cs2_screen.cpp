#include "ui/screens/cs2_screen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/cs2_gc/cs2_gc_client.hpp"
#include "core/steam_login/session.hpp"
#include "core/strings.hpp"
#include "ui/screens/cs2_screen_state.hpp"
#include "ui/theme.hpp"
#include "ui/util.hpp"
#include "ui/widgets/avatar_cache.hpp"
#include "ui/widgets/redacted_text.hpp"

namespace sam::ui::screens {

namespace {

// True when this account's weekly drop has been claimed for the current weekly period.
// The GC has no explicit "claimed" flag, so the loaded store's generation_time is the source
// of truth: it is set when a drop is granted on rank-up, so a zero balance whose generation
// falls in the current week (since the last Wednesday reset) means this week's drop was taken.
// Crucially the GC keeps showing last week's drop after the reset -- same generation, zero
// balance -- until a new one is granted, so a pre-reset generation must NOT count as claimed;
// that is what resets the card at the week boundary. The persistent reset marker is kept in
// step with this in on_snapshot, since account-list contexts have no live store of their own.
bool drop_claimed_this_week(const cs2_gc::WeeklyReward& reward) {
    if (!reward.loaded || reward.available) return false;
    const std::int64_t now = now_seconds();
    const std::int64_t last_reset = next_weekly_reset(now) - 7 * 86400;
    return reward.generation_time >= static_cast<std::uint32_t>(last_reset);
}

// Persona name, or the privacy-aware login when there is no persona, matching the label
// other screens show for an account.
std::string cs2_account_label(const app::AppState& state, const core::Account& a) {
    return a.web.persona_name.empty() ? widgets::login_label(state, a) : a.web.persona_name;
}

// Header chip showing the selected account (avatar + name). Returns true when clicked.
bool draw_cs2_account_chip(const app::AppState& state, const core::Account* picked) {
    constexpr float kH = 40.0F;
    constexpr float kW = 260.0F;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##cs2_acct_chip", ImVec2(kW, kH));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p1(p0.x + kW, p0.y + kH);
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1,
                      ImGui::ColorConvertFloat4ToU32(hov ? theme::panel_hover() : theme::panel()),
                      6.0F);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::border()), 6.0F, 1.0F);
    const float pad = 6.0F;
    const float av = kH - 2.0F * pad;
    const ImVec2 av0(p0.x + pad, p0.y + pad);
    const ImVec2 av1(av0.x + av, av0.y + av);
    const float av_round = av * (8.0F / 48.0F);
    dl->AddRectFilled(av0, av1, IM_COL32(40, 50, 60, 180), av_round);
    if (picked != nullptr)
        if (auto* srv = widgets::avatar_for(picked->web.avatar_url_full))
            dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv), av0, av1, ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, av_round);
    const std::string label =
        picked != nullptr ? cs2_account_label(state, *picked) : std::string("Select an account");
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(av1.x + 8.0F, p0.y + (kH - ts.y) * 0.5F),
                ImGui::ColorConvertFloat4ToU32(picked != nullptr ? theme::text() : theme::dim_text()),
                label.c_str());
    return clicked;
}

// Single-select account grid for the header popup: filterable chips with avatar + name.
// Clicking a chip points the screen at that account and closes the popup. Adapted from the
// trade-offer picker (trade_offers_modals.cpp), single-select instead of multi.
void draw_cs2_account_picker(app::AppState& state, char (&search)[64]) {
    ImGui::SetNextItemWidth(220.0F);
    ImGui::InputTextWithHint("##cs2acctsearch", "filter", search, sizeof(search));

    const std::string af = core::to_lower(search);
    std::vector<const core::Account*> accts;
    for (auto& a : state.vault.accounts) {
        if (a.steam_id_64 == 0) continue;  // the GC needs a resolved SteamID
        if (!af.empty() &&
            core::to_lower(cs2_account_label(state, a)).find(af) == std::string::npos &&
            core::to_lower(a.login).find(af) == std::string::npos)
            continue;
        accts.push_back(&a);
    }

    constexpr float kChipH = 44.0F;
    constexpr float kChipGap = 8.0F;
    ImGui::BeginChild("##cs2acctgrid", ImVec2(420.0F, 280.0F), ImGuiChildFlags_Borders);
    const float gw = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>(gw / 220.0F + 0.5F));
    const float chip_w = (gw - static_cast<float>(cols - 1) * kChipGap) / static_cast<float>(cols);
    const float row_pitch = kChipH + ImGui::GetStyle().ItemSpacing.y;
    const int total = static_cast<int>(accts.size());
    const int rows = (total + cols - 1) / cols;
    auto* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(rows, row_pitch);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int c = 0; c < cols; ++c) {
                const int idx = row * cols + c;
                if (idx >= total) break;
                const core::Account& a = *accts[idx];
                if (c > 0) ImGui::SameLine(0.0F, kChipGap);
                ImGui::PushID(a.id.c_str());
                const bool sel = state.selected_account_id == a.id;
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p1(p0.x + chip_w, p0.y + kChipH);
                if (ImGui::InvisibleButton("##chip", ImVec2(chip_w, kChipH))) {
                    state.selected_account_id = a.id;
                    ImGui::CloseCurrentPopup();
                }
                const bool hov = ImGui::IsItemHovered();
                ImU32 bg;
                if (sel) {
                    ImVec4 ac = theme::accent();
                    ac.w = 0.20F;
                    bg = ImGui::ColorConvertFloat4ToU32(ac);
                } else {
                    bg = ImGui::ColorConvertFloat4ToU32(hov ? theme::panel_hover() : theme::panel());
                }
                dl->AddRectFilled(p0, p1, bg, 6.0F);

                const float pad = 6.0F;
                const float av = kChipH - 2.0F * pad;
                const ImVec2 av0(p0.x + pad, p0.y + pad);
                const ImVec2 av1(av0.x + av, av0.y + av);
                const float av_round = av * (8.0F / 48.0F);
                dl->AddRectFilled(av0, av1, IM_COL32(40, 50, 60, 180), av_round);
                if (auto* srv = widgets::avatar_for(a.web.avatar_url_full))
                    dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv), av0, av1, ImVec2(0, 0),
                                        ImVec2(1, 1), IM_COL32_WHITE, av_round);

                const float tx0 = av1.x + 8.0F;
                const float tx1 = p1.x - pad;
                const std::string label = cs2_account_label(state, a);
                const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                const float region_w = tx1 - tx0;
                const float text_x = tx0 + std::max(0.0F, (region_w - ts.x) * 0.5F);
                const float text_y = p0.y + (kChipH - ts.y) * 0.5F;
                dl->PushClipRect(ImVec2(tx0, p0.y), ImVec2(tx1, p1.y), true);
                dl->AddText(ImVec2(text_x, text_y), ImGui::ColorConvertFloat4ToU32(theme::text()),
                            label.c_str());
                dl->PopClipRect();
                if (sel)
                    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(theme::accent()), 6.0F, 2.0F);
                if (hov && ts.x > region_w) set_tooltip("%s", label.c_str());
                ImGui::PopID();
            }
        }
    }
    clipper.End();
    ImGui::EndChild();
}

void start_client(app::AppState& state, const core::Account& acct) {
    auto& s = *state.cs2_screen;
    s.error.clear();
    s.connected = false;
    s.snapshot = {};
    s.open_unit = 0;
    s.unit_contents.clear();
    s.status = "Starting";

    cs2_gc::Cs2Credentials creds;
    creds.refresh_token = app::cs2_client_token(acct);
    creds.steam_id = acct.steam_id_64;
    creds.proxy = std::string(acct.proxy.begin(), acct.proxy.end());
    creds.claimed_generation = acct.cs2.weekly_drop_claimed_generation;
    creds.claimed_ids = acct.cs2.weekly_drop_claimed_ids;
    creds.claimed_names = acct.cs2.weekly_drop_claimed_names;

    app::AppState* st = &state;
    const std::string aid = acct.id;

    auto valid = [st, aid] { return st->cs2_screen && st->cs2_screen->account_id == aid; };

    cs2_gc::Cs2Callbacks cb;
    cb.on_status = [st, aid, valid](std::string text) {
        st->post_ui_callback([st, aid, valid, text = std::move(text)] {
            if (!valid()) return;
            st->cs2_screen->status = text;
            st->cs2_screen->connected = (text == "Ready");
        });
    };
    cb.on_snapshot = [st, aid, valid](cs2_gc::Snapshot snap) {
        st->post_ui_callback([st, aid, valid, snap = std::move(snap)]() mutable {
            if (!valid()) return;
            st->cs2_screen->snapshot = std::move(snap);
            st->apply_gc_snapshot_cache(aid, st->cs2_screen->snapshot);
            // Keep the persistent weekly-drop marker (what the account list reads, with no
            // live store of its own) in step with the GC store: light it once this week's drop
            // is taken, and clear it after a new week has reset even while the GC still shows
            // last week's drop. Gated on the setting; manual marking owns it when off.
            core::Account* acc = st->find_account(aid);
            if (acc == nullptr) return;
            const cs2_gc::WeeklyReward& reward = st->cs2_screen->snapshot.reward;
            if (st->settings.cs2_gc.auto_mark_claimed && reward.loaded && !reward.available) {
                const std::int64_t now = now_seconds();
                const bool claimed = drop_claimed_this_week(reward);
                const bool marked = acc->cs2.weekly_drop_reset_unix > now;
                if (claimed != marked) {
                    acc->cs2.weekly_drop_reset_unix = claimed ? next_weekly_reset(now) : 0;
                    st->vault_dirty = true;
                    st->save_vault_if_dirty();
                }
            }
        });
    };
    cb.on_unit_contents = [st, aid, valid](std::uint64_t unit, std::vector<cs2_gc::DisplayItem> items) {
        st->post_ui_callback([st, aid, valid, unit, items = std::move(items)]() mutable {
            if (!valid()) return;
            auto& cs = *st->cs2_screen;
            cs.unit_loading = false;
            if (cs.open_unit == unit) cs.unit_contents = std::move(items);
        });
    };
    cb.on_error = [st, aid, valid](std::string err) {
        st->post_ui_callback([st, aid, valid, err = std::move(err)] {
            if (!valid()) return;
            auto& cs = *st->cs2_screen;
            cs.error = err;
            cs.action_busy = false;
            cs.unit_loading = false;
        });
    };
    cb.on_action_done = [st, aid, valid](std::string msg) {
        st->post_ui_callback([st, aid, valid, msg = std::move(msg)] {
            if (!valid()) return;
            auto& cs = *st->cs2_screen;
            cs.action_busy = false;
            cs.error.clear();
            cs.status = msg;
        });
    };
    // Cache what the weekly drop picked into the vault. Not gated on the screen still
    // showing this account (valid()): the claim already happened, so persist it as long
    // as the account exists, and also mark the weekly-drop reset so the account-list
    // marker lights up like the manual "Mark claimed" menu does.
    cb.on_reward_claimed = [st, aid](std::uint32_t gen, std::vector<std::uint64_t> ids,
                                     std::vector<std::string> names) {
        st->post_ui_callback([st, aid, gen, ids = std::move(ids),
                              names = std::move(names)]() mutable {
            auto* acc = st->find_account(aid);
            if (!acc) return;
            acc->cs2.weekly_drop_claimed_generation = gen;
            acc->cs2.weekly_drop_claimed_ids = std::move(ids);
            acc->cs2.weekly_drop_claimed_names = std::move(names);
            acc->cs2.weekly_drop_reset_unix = next_weekly_reset(now_seconds());
            st->vault_dirty = true;
            st->save_vault_if_dirty();
        });
    };

    s.client = std::make_unique<cs2_gc::Cs2GcClient>(std::move(creds), std::move(cb));
}

// Hand the live GC client to the background reaper so account/tab switches and
// Disconnect never block the render thread joining its worker.
void discard_client(Cs2ScreenState& s) {
    if (s.client) cs2_gc::retire(std::move(s.client));
}

// One selectable item tile (rounded panel, inset icon, rarity-colored border, optional
// selection highlight, storage-unit count badge, name-on-hover tooltip). Returns true
// when clicked; advances the cursor by w x h.
bool draw_tile(const cs2_gc::DisplayItem& it, float w, float h, bool selected) {
    const float inset = h * 0.12F;
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(w, h));
    const ImVec2 c1(c0.x + w, c0.y + h);
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(c0, c1, ImGui::ColorConvertFloat4ToU32(theme::panel_hover()), 6.0F);
    if (!it.icon_url.empty())
        if (auto* srv = widgets::texture_for(it.icon_url))
            dl->AddImageRounded(reinterpret_cast<ImTextureID>(srv),
                                ImVec2(c0.x + inset, c0.y + inset),
                                ImVec2(c1.x - inset, c1.y - inset), ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32_WHITE, 4.0F);
    const ImU32 border = it.border_rgb != 0
                             ? IM_COL32((it.border_rgb >> 16) & 0xFF, (it.border_rgb >> 8) & 0xFF,
                                        it.border_rgb & 0xFF, 255)
                             : ImGui::ColorConvertFloat4ToU32(theme::border());
    // Vendored ImGui swapped AddRect thickness<->flags; pass thickness before flags.
    dl->AddRect(c0, c1, border, 6.0F, 2.0F);
    if (selected)
        dl->AddRect(c0, c1, ImGui::ColorConvertFloat4ToU32(theme::accent()), 6.0F, 3.0F);
    if (it.is_storage_unit && it.item_count >= 0) {
        char cnt[24];
        std::snprintf(cnt, sizeof(cnt), "%d items", it.item_count);
        dl->AddText(ImVec2(c0.x + inset, c1.y - inset - ImGui::GetTextLineHeight()), IM_COL32_WHITE,
                    cnt);
    }
    if (ImGui::IsItemHovered() && !it.name.empty()) set_tooltip("%s", it.name.c_str());
    return clicked;
}

// Lays out `items` as a cols x rows paginated grid (tiles fill the column width, capped)
// with a Page nav row. is_selected() drives the highlight; on_click() fires per click.
template <class SelFn, class ClickFn>
void draw_grid(const std::vector<cs2_gc::DisplayItem>& items, int& page, int cols, int rows,
               SelFn is_selected, ClickFn on_click) {
    const int per_page = cols * rows;
    const int total = static_cast<int>(items.size());
    const int pages = (total + per_page - 1) / per_page;
    page = std::clamp(page, 0, std::max(0, pages - 1));

    constexpr float gap = 6.0F;
    const float avail = ImGui::GetContentRegionAvail().x;
    float tile_w = (avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    tile_w = std::clamp(tile_w, 56.0F, 132.0F);
    const float tile_h = tile_w * 0.72F;

    const int start = page * per_page;
    for (int i = 0; i < per_page; ++i) {
        if (i % cols > 0) ImGui::SameLine(0.0F, gap);
        const int idx = start + i;
        if (idx >= total) {
            ImGui::Dummy(ImVec2(tile_w, tile_h));
            continue;
        }
        const auto& it = items[idx];
        ImGui::PushID(reinterpret_cast<void*>(static_cast<std::uintptr_t>(it.id)));
        if (draw_tile(it, tile_w, tile_h, is_selected(it))) on_click(it);
        ImGui::PopID();
    }

    if (pages > 1) {
        ImGui::BeginDisabled(page <= 0);
        if (ImGui::SmallButton("<")) --page;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Page %d/%d", page + 1, pages);
        ImGui::SameLine();
        ImGui::BeginDisabled(page >= pages - 1);
        if (ImGui::SmallButton(">")) ++page;
        ImGui::EndDisabled();
    }
}

// A progress bar with its label centered over the whole bar (ImGui's built-in overlay
// hugs the fill edge). Pass "" as the overlay to draw the bar alone, then stamp the text
// at the rect center via the draw list.
void progress_bar_centered(float frac, float width, const char* text) {
    frac = std::clamp(frac, 0.0F, 1.0F);
    ImGui::ProgressBar(frac, ImVec2(width, 0.0F), "");
    const ImVec2 r0 = ImGui::GetItemRectMin();
    const ImVec2 r1 = ImGui::GetItemRectMax();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(r0.x + (r1.x - r0.x - ts.x) * 0.5F, r0.y + (r1.y - r0.y - ts.y) * 0.5F),
        ImGui::GetColorU32(ImGuiCol_Text), text);
}

void draw_units(Cs2ScreenState& s, float width) {
    ImGui::BeginChild("##cs2_units", ImVec2(width, 0.0F),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    if (s.open_unit != 0) {
        const cs2_gc::DisplayItem* open = nullptr;
        for (const auto& u : s.snapshot.units)
            if (u.id == s.open_unit) { open = &u; break; }
        ImGui::TextUnformatted(open != nullptr ? open->name.c_str() : "Storage unit");
        ImGui::SameLine();
        if (ImGui::SmallButton("Close")) {
            s.open_unit = 0;
            s.unit_contents.clear();
            s.selected_unit.clear();
        }
        ImGui::Separator();
        if (s.unit_loading) {
            ImGui::TextDisabled("Loading contents...");
        } else if (s.unit_contents.empty()) {
            ImGui::TextDisabled("(empty)");
        } else {
            draw_grid(
                s.unit_contents, s.contents_page, 5, 5,
                [&](const cs2_gc::DisplayItem& it) { return s.selected_unit.count(it.id) != 0; },
                [&](const cs2_gc::DisplayItem& it) {
                    if (s.selected_unit.count(it.id) != 0) s.selected_unit.erase(it.id);
                    else s.selected_unit.insert(it.id);
                });
            ImGui::Spacing();
            char lbl[40];
            std::snprintf(lbl, sizeof(lbl), "Move out selected (%zu)", s.selected_unit.size());
            ImGui::BeginDisabled(s.action_busy || s.selected_unit.empty());
            if (ImGui::SmallButton(lbl) && s.client) {
                s.action_busy = true;
                s.error.clear();
                s.client->move_out_many(
                    s.open_unit,
                    std::vector<std::uint64_t>(s.selected_unit.begin(), s.selected_unit.end()));
                s.selected_unit.clear();
                s.unit_loading = true;  // refresh the contents grid after the batch runs
                s.contents_page = 0;
                s.client->load_unit(s.open_unit);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(s.selected_unit.empty());
            if (ImGui::SmallButton("Clear")) s.selected_unit.clear();
            ImGui::EndDisabled();
        }
    } else {
        ImGui::TextUnformatted("Storage units");
        ImGui::Separator();
        if (s.snapshot.units.empty()) {
            ImGui::TextDisabled("No storage units in this inventory.");
        } else {
            draw_grid(
                s.snapshot.units, s.units_page, 5, 5,
                [](const cs2_gc::DisplayItem&) { return false; },
                [&](const cs2_gc::DisplayItem& it) {
                    if (s.client == nullptr) return;
                    s.open_unit = it.id;
                    s.unit_contents.clear();
                    s.unit_loading = true;
                    s.contents_page = 0;
                    s.selected_unit.clear();
                    s.client->load_unit(it.id);
                });
        }
    }
    ImGui::EndChild();
}

void draw_inventory(Cs2ScreenState& s, float width) {
    ImGui::BeginChild("##cs2_inv", ImVec2(width, 0.0F),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::Text("Inventory (%zu)", s.snapshot.items.size());
    ImGui::Separator();
    if (s.snapshot.items.empty()) {
        ImGui::TextDisabled("No storable items.");
    } else {
        draw_grid(
            s.snapshot.items, s.inv_page, 5, 5,
            [&](const cs2_gc::DisplayItem& it) { return s.selected_inv.count(it.id) != 0; },
            [&](const cs2_gc::DisplayItem& it) {
                if (s.selected_inv.count(it.id) != 0) s.selected_inv.erase(it.id);
                else s.selected_inv.insert(it.id);
            });
        ImGui::Spacing();
        const bool can_store = s.open_unit != 0;
        char lbl[40];
        std::snprintf(lbl, sizeof(lbl), "Store selected (%zu)", s.selected_inv.size());
        ImGui::BeginDisabled(s.action_busy || s.selected_inv.empty() || !can_store);
        if (ImGui::SmallButton(lbl) && s.client) {
            s.action_busy = true;
            s.error.clear();
            s.client->move_in_many(
                s.open_unit,
                std::vector<std::uint64_t>(s.selected_inv.begin(), s.selected_inv.end()));
            s.selected_inv.clear();
            s.unit_loading = true;  // refresh the open unit's contents after the batch runs
            s.contents_page = 0;
            s.client->load_unit(s.open_unit);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(s.selected_inv.empty());
        if (ImGui::SmallButton("Clear")) s.selected_inv.clear();
        ImGui::EndDisabled();
        if (!can_store) {
            ImGui::SameLine();
            ImGui::TextDisabled("Open a storage unit first");
        }
    }
    ImGui::EndChild();
}

// Weekly-drop card: the claim UI while a drop is redeemable, else the level-progress bar
// plus the claimed/not-ready status (see drop_claimed_this_week for the detection).
void draw_weekly_drop(Cs2ScreenState& s, float width) {
    ImGui::BeginChild("##cs2_drop", ImVec2(width, 0.0F),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Weekly drop");
    ImGui::Separator();

    const cs2_gc::WeeklyReward& reward = s.snapshot.reward;
    const float bar_w = ImGui::GetContentRegionAvail().x;
    if (reward.available) {
        ImGui::Text("Weekly drop ready: %u to claim", reward.redeemable_balance);
        const std::size_t cap = reward.redeemable_balance;
        // When you can take every candidate there's nothing to choose -- show a plain
        // preview. Otherwise let the user pick which `cap` items to keep.
        const bool claim_all = reward.items.size() <= cap;
        draw_grid(
            reward.items, s.reward_page, 5, 1,
            [&](const cs2_gc::DisplayItem& it) {
                return !claim_all && s.selected_reward.count(it.id) != 0;
            },
            [&](const cs2_gc::DisplayItem& it) {
                if (claim_all) return;  // preview only
                if (s.selected_reward.count(it.id) != 0) s.selected_reward.erase(it.id);
                else if (s.selected_reward.size() < cap) s.selected_reward.insert(it.id);
            });
        ImGui::Spacing();
        std::vector<std::uint64_t> ids;
        if (claim_all) {
            ids = reward.item_ids;
            if (ids.size() > cap) ids.resize(cap);
        } else {
            ids.assign(s.selected_reward.begin(), s.selected_reward.end());
        }
        char lbl[40];
        if (claim_all)
            std::snprintf(lbl, sizeof(lbl), "Claim");
        else
            std::snprintf(lbl, sizeof(lbl), "Claim (%zu/%u)", s.selected_reward.size(),
                          reward.redeemable_balance);
        ImGui::BeginDisabled(s.action_busy || ids.empty());
        if (ImGui::SmallButton(lbl)) {
            s.action_busy = true;
            s.error.clear();
            s.selected_reward.clear();
            s.client->claim_reward(std::move(ids));
        }
        ImGui::EndDisabled();
    } else {
        // A headline line above the bar so this card's progress bar lines up with the
        // mission card's (which has its name line above its bar).
        const bool claimed = drop_claimed_this_week(reward);
        std::string names;
        if (claimed)
            for (const std::string& n : reward.claimed_names) {
                if (n.empty()) continue;
                if (!names.empty()) names += ", ";
                names += n;
            }
        if (claimed)
            ImGui::TextWrapped("Weekly drop claimed this week.");
        else if (reward.loaded)
            ImGui::TextWrapped("Rank up to unlock the weekly drop.");
        else
            ImGui::TextWrapped("No weekly drop to claim right now.");

        const cs2_gc::PlayerProgress& prog = s.snapshot.progress;
        if (prog.valid) {
            const float frac = prog.xp_per_level > 0
                                   ? static_cast<float>(prog.xp_in_level) / prog.xp_per_level
                                   : 0.0F;
            char overlay[48];
            std::snprintf(overlay, sizeof(overlay), "Level %d  -  %d / %d XP", prog.level,
                          prog.xp_in_level, prog.xp_per_level);
            progress_bar_centered(frac, bar_w, overlay);
        }

        // Secondary note below the bar (mirrors the mission card's dim note line). Show the
        // reset date whenever the store is loaded (claimed or just not earned yet), plus the
        // picked items once claimed.
        if (reward.loaded) {
            if (claimed && !names.empty()) ImGui::TextDisabled("Picked: %s", names.c_str());
            char when[32] = "";
            const auto next = static_cast<std::time_t>(next_weekly_reset(now_seconds()));
            std::tm tm{};
            if (localtime_s(&tm, &next) == 0 && std::strftime(when, sizeof(when), "%b %d", &tm) > 0)
                ImGui::TextDisabled("Resets around %s.", when);
        }
    }
    ImGui::EndChild();
}

// Weekly recurring mission card: name + centered progress bar. The XP is granted by the
// game server as you play; there is no GC message to claim it, hence the note (no button).
void draw_weekly_mission(Cs2ScreenState& s, float width) {
    ImGui::BeginChild("##cs2_mission", ImVec2(width, 0.0F),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Weekly mission");
    ImGui::Separator();
    const cs2_gc::WeeklyMission& mission = s.snapshot.mission;
    if (!mission.valid) {
        ImGui::TextDisabled("No weekly mission right now.");
    } else {
        if (!mission.name.empty()) ImGui::TextWrapped("%s", mission.name.c_str());
        const float bar_w = ImGui::GetContentRegionAvail().x;
        if (mission.target > 0) {
            const float frac =
                static_cast<float>(mission.progress) / static_cast<float>(mission.target);
            char overlay[48];
            if (mission.xp > 0)
                std::snprintf(overlay, sizeof(overlay), "%u / %u  -  %u XP", mission.progress,
                              mission.target, mission.xp);
            else
                std::snprintf(overlay, sizeof(overlay), "%u / %u", mission.progress, mission.target);
            progress_bar_centered(frac, bar_w, overlay);
        } else {
            ImGui::Text("Progress: %u", mission.progress);
        }
        ImGui::TextDisabled("Earned automatically by playing.");
    }
    ImGui::EndChild();
}

}  // namespace

void discard_cs2_client(app::AppState& state) {
    if (state.cs2_screen) discard_client(*state.cs2_screen);
    state.cs2_screen.reset();
}

void draw_cs2(app::AppState& state) {
    static char picker_search[64] = "";

    ImGui::TextUnformatted("Account");
    core::Account* picked = state.selected_account_id.empty()
                                ? nullptr
                                : state.find_account(state.selected_account_id);
    if (draw_cs2_account_chip(state, picked)) {
        picker_search[0] = '\0';
        ImGui::OpenPopup("##cs2_account_picker");
    }
    if (begin_styled_popup("##cs2_account_picker")) {
        draw_cs2_account_picker(state, picker_search);
        end_styled_popup();
    }
    ImGui::Spacing();

    ImGui::TextUnformatted("Connection");
    separator();

    if (state.selected_account_id.empty()) {
        if (state.cs2_screen) discard_client(*state.cs2_screen);
        state.cs2_screen.reset();
        ImGui::TextDisabled("Pick an account above to manage its CS2 inventory.");
        return;
    }
    core::Account* acct = state.find_account(state.selected_account_id);
    if (acct == nullptr || acct->steam_id_64 == 0) {
        if (state.cs2_screen) discard_client(*state.cs2_screen);
        state.cs2_screen.reset();
        ImGui::TextDisabled("This account has no resolved Steam ID yet. Refresh it first.");
        return;
    }

    if (!state.cs2_screen || state.cs2_screen->account_id != acct->id) {
        if (state.cs2_screen) discard_client(*state.cs2_screen);
        state.cs2_screen = std::make_unique<Cs2ScreenState>();
        state.cs2_screen->account_id = acct->id;
    }
    Cs2ScreenState& s = *state.cs2_screen;

    if (app::cs2_client_token(*acct).empty()) {
        if (acct->password.empty()) {
            ImGui::TextWrapped("This account needs a one-time CS2 client sign-in, but no password is "
                               "stored. Add it through Full Login first.");
            return;
        }
        ImGui::TextWrapped("This account uses a mobile-scoped token, which Steam does not accept for "
                           "the CS2 Game Coordinator. A one-time client sign-in is needed; Steam Guard "
                           "is filled automatically from the in-app authenticator.");
        ImGui::Spacing();
        ImGui::BeginDisabled(s.acquiring);
        if (ImGui::Button("Set up CS2 sign-in")) {
            s.acquiring = true;
            s.error.clear();
            app::AppState* st = &state;
            const std::string aid = acct->id;
            state.acquire_cm_token(aid, [st, aid](bool ok, std::string err) {
                if (!st->cs2_screen || st->cs2_screen->account_id != aid) return;
                st->cs2_screen->acquiring = false;
                if (!ok) st->cs2_screen->error = std::move(err);
            });
        }
        ImGui::EndDisabled();
        if (s.acquiring) {
            ImGui::SameLine();
            ImGui::TextDisabled("Signing in...");
        }
        if (!s.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.45F, 0.45F, 1.0F));
            ImGui::TextWrapped("%s", s.error.c_str());
            ImGui::PopStyleColor();
        }
        return;
    }

    if (!s.client) {
        if (ImGui::Button("Connect to CS2")) start_client(state, *acct);
        if (!s.status.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", s.status.c_str());
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                               kContentPaddingX);
        ImGui::TextWrapped("Connecting takes over this account's CS2 session and disconnects any "
                           "other connected account. If it is in a match it will be kicked, so "
                           "avoid connecting while it is playing.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::TextUnformatted(s.status.c_str());
    ImGui::SameLine();
    if (s.connected) {
        // Manual re-pull of the inventory/store (e.g. after buying skins). Live changes
        // also refresh on their own; this is for when you want to force it.
        if (ImGui::SmallButton("Refresh") && s.client) s.client->refresh();
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Disconnect")) {
        discard_client(s);
        s.connected = false;
        s.status = "Disconnected";
        return;
    }

    if (!s.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.45F, 0.45F, 1.0F));
        ImGui::TextWrapped("%s", s.error.c_str());
        ImGui::PopStyleColor();
    }

    if (!s.connected) {
        ImGui::TextDisabled("Working...");
        return;
    }

    ImGui::Spacing();
    // Reserve a right gutter equal to the left indent so the cards sit symmetrically in the
    // page (PushItemWidth's inset applies to items, not to child windows).
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x - kContentPaddingX;
    const float col = (avail - gap) * 0.5F;

    // Only the cards enabled in settings, laid out two-up in their original order
    // (drop, mission, then units, inventory) so hiding one never leaves a dangling
    // SameLine.
    const auto& gc = state.settings.cs2_gc;
    std::vector<std::function<void(float)>> cards;
    if (gc.show_weekly_drop)    cards.push_back([&](float w) { draw_weekly_drop(s, w); });
    if (gc.show_weekly_mission) cards.push_back([&](float w) { draw_weekly_mission(s, w); });
    if (gc.show_storage_units)  cards.push_back([&](float w) { draw_units(s, w); });
    if (gc.show_inventory)      cards.push_back([&](float w) { draw_inventory(s, w); });

    // The page sets ItemSpacing.y to 10 for section spacing, which otherwise
    // leaks into the cards and their fixed 5-row grids and pushes the whole page
    // into a few-pixel scroll. Use the theme's 8px row spacing inside the cards;
    // the unchanged x keeps the two-up gap that col was sized against.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 8.0F));
    for (std::size_t i = 0; i < cards.size(); i += 2) {
        if (i > 0) ImGui::Spacing();
        cards[i](col);
        if (i + 1 < cards.size()) {
            ImGui::SameLine();
            cards[i + 1](col);
        }
    }
    ImGui::PopStyleVar();
}

}  // namespace sam::ui::screens
