#include "core/cs2_gc/gc_session.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>

#include "core/cs2_gc/gc_emsg.hpp"
#include "core/cs2_gc/gc_message.hpp"
#include "core/log.hpp"
#include "core/steam_auth/gen/base_gcmessages.pb.h"
#include "core/steam_auth/gen/cstrike15_gcmessages.pb.h"
#include "core/steam_auth/gen/econ_gcmessages.pb.h"
#include "core/steam_auth/gen/gcsdk_gcmessages.pb.h"
#include "core/steam_auth/gen/steammessages_clientserver_extra.pb.h"
#include "core/steam_cm/emsg.hpp"

namespace sam::cs2_gc {

using clock = std::chrono::steady_clock;

namespace {
std::string handshake_drop_reason(int logoff_eresult) {
    if (logoff_eresult == 6)  // LoggedInElsewhere
        return "this account is signed in elsewhere. An hourboost (or the Steam client) is holding "
               "the CS2 session; stop it for this account, then reconnect.";
    if (logoff_eresult != 0)
        return "Steam ended the session (EResult " + std::to_string(logoff_eresult) + ")";
    return "connection dropped during the CS2 handshake";
}

// Lowercase hex of a byte blob, for logging raw GC payloads we can decode by hand.
std::string hex_dump(const std::string& data) {
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(data.size() * 2);
    for (unsigned char c : data) {
        hex += kHex[c >> 4];
        hex += kHex[c & 0x0f];
    }
    return hex;
}

// The recurring-mission templates are Valve binary KeyValues: 0x01 = string field
// (key\0value\0), 0x02 = int field (key\0 + 4 raw bytes), 0x00/0x0b open/close blocks. We
// only need a few string fields for the account's mission, so pull each by its
// "\x01<key>\0" tag searched forward from the mission's "\x00<id>\0" block marker.
std::string field_after(const std::string& blob, std::size_t from, const std::string& key) {
    const auto p = blob.find(key, from);
    if (p == std::string::npos) return {};
    const std::size_t s = p + key.size();
    const std::size_t e = blob.find('\0', s);
    return blob.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

std::uint32_t list_max(const std::string& csv) {
    std::uint32_t m = 0;
    for (std::size_t i = 0; i < csv.size();) {
        const auto v = static_cast<std::uint32_t>(std::strtoul(csv.c_str() + i, nullptr, 10));
        if (v > m) m = v;
        const auto c = csv.find(',', i);
        if (c == std::string::npos) break;
        i = c + 1;
    }
    return m;
}

std::uint32_t list_sum(const std::string& csv) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < csv.size();) {
        sum += static_cast<std::uint32_t>(std::strtoul(csv.c_str() + i, nullptr, 10));
        const auto c = csv.find(',', i);
        if (c == std::string::npos) break;
        i = c + 1;
    }
    return sum;
}

std::string title_case(std::string s) {
    bool start = true;
    for (char& ch : s) {
        if (ch == '_' || ch == ' ') {
            ch = ' ';
            start = true;
        } else if (start) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            start = false;
        }
    }
    return s;
}

// CS2 quest expression -> a human label like the in-game card ("Round Wins", "Kills"). The
// expression may be compound, e.g. "act_kill_human && !![weapon ak47|weapon awp|...]" for kills
// restricted to a weapon set; keep only the leading act_* token so the card reads "Kills"
// instead of dumping the weapon list.
std::string action_label(const std::string& expr) {
    const std::string base = expr.substr(0, expr.find_first_of(" \t&|!["));
    if (base == "act_win_round") return "Round Wins";
    if (base == "act_kill_human") return "Kills";
    if (base == "act_kill_chicken") return "Chicken Kills";
    std::string e = base.rfind("act_", 0) == 0 ? base.substr(4) : base;
    return title_case(e);
}

std::string map_label(std::string m) {
    for (const char* pre : {"de_", "cs_", "ar_", "dz_"})
        if (m.rfind(pre, 0) == 0) {
            m = m.substr(3);
            break;
        }
    return title_case(m);
}
}  // namespace

bool GcSession::launch(std::string& error) {
    dropped_ = false;

    // Steam pushes the playing-session state right after logon; proceed the moment it
    // arrives instead of always burning the full window.
    wait_until([this] { return cm_.playing_state_seen(); }, 1500);

    // If the account is already playing CS2 elsewhere (e.g. an hourbooster), take
    // over the game session the way the Steam client's "start anyway" prompt does.
    if (cm_.playing_blocked()) {
        SAM_LOG_INFO("gc: account is playing elsewhere, taking over the game session");
        cm_.kick_playing_session();
        const auto kick_deadline = clock::now() + std::chrono::seconds(10);
        while (cm_.playing_blocked() && clock::now() < kick_deadline) {
            if (!pump(500)) {
                dropped_ = true;
                error = handshake_drop_reason(cm_.last_logoff_eresult());
                return false;
            }
        }
    }

    CMsgClientGamesPlayed gp;
    gp.add_games_played()->set_game_id(kCs2AppId);
    if (!cm_.send(steam_cm::EMsg::ClientGamesPlayedWithDataBlob, gp)) {
        dropped_ = true;
        error = handshake_drop_reason(cm_.last_logoff_eresult());
        return false;
    }

    // Let games_played propagate, then start the hello loop. The GC harmlessly ignores
    // a hello that lands a touch early, so a short settle beats a fixed 2s wait.
    pump_for(300);

    const auto start = clock::now();
    const auto deadline = start + std::chrono::seconds(90);
    auto next_hello = clock::now();
    int hello_count = 0;
    while (!welcome_received_ && clock::now() < deadline) {
        if (clock::now() >= next_hello) {
            send_hello();
            ++hello_count;
            next_hello = clock::now() + std::chrono::milliseconds(1000);
        }
        if (!pump(500)) {
            dropped_ = true;
            error = handshake_drop_reason(cm_.last_logoff_eresult());
            return false;
        }
    }
    if (!welcome_received_) {
        error = "CS2's Game Coordinator did not respond. The account may not own CS2, "
                "or the GC is busy; try again.";
        return false;
    }
    SAM_LOG_INFO("gc: welcome after {} hello(s), {} ms", hello_count,
                 std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count());
    // Pull the account's XP/level for the weekly-drop progress bar (best-effort). The
    // recurring (weekly) mission progress arrives via the SO cache; request its schedule
    // too so the GC sends the schema and any fresh mission state.
    request_players_profile();
    request_mission_schedule();
    wait_until([this] { return player_xp_.valid; }, 3000);
    return true;
}

void GcSession::send_hello() {
    CMsgClientHello hello;
    hello.set_version(kGcHelloVersion);
    send_gc(GcMsg::ClientHello, hello);
}

void GcSession::request_profile(std::uint32_t account_id) {
    CMsgGCCStrike15_v2_ClientRequestPlayersProfile req;
    req.set_account_id(account_id);
    req.set_request_level(32);
    send_gc(GcMsg::RequestPlayersProfile, req);
}

void GcSession::request_players_profile() {
    request_profile(static_cast<std::uint32_t>(cm_.steam_id() & 0xffffffffULL));
}

std::vector<ProfileResult> GcSession::take_profiles() {
    std::vector<ProfileResult> out;
    out.reserve(profile_results_.size());
    for (auto& r : profile_results_) out.push_back(std::move(r));
    profile_results_.clear();
    return out;
}

void GcSession::request_mission_schedule() {
    CMsgRequestRecurringMissionSchedule req;
    send_gc(GcMsg::RequestRecurringMissionSchedule, req);
}

MissionInfo GcSession::current_mission() const {
    MissionInfo out;
    const std::string& tpl = current_period_templates_;
    if (tpl.size() < 2 || tpl[0] != '\0') return out;  // need at least one mission block

    // Pick the mission: the account's mission-SO id when the SO belongs to THIS period and its
    // id is in the template, else the first mission the template lists (the week's default for
    // an account whose current-period SO hasn't arrived yet). Gating on the period stops a
    // stale older-week SO from selecting last week's mission after the Wednesday reset.
    std::size_t start = std::string::npos;
    std::uint32_t mission_id = 0;
    if (recurring_mission_.valid && recurring_mission_.period == current_period_) {
        std::string marker(1, '\0');
        marker += std::to_string(recurring_mission_.mission_id);
        marker += '\0';
        const auto p = tpl.find(marker);
        if (p != std::string::npos) {
            start = p;
            mission_id = recurring_mission_.mission_id;
        }
    }
    if (start == std::string::npos) {
        const auto e = tpl.find('\0', 1);  // first block: leading 0x00 then the id string
        if (e == std::string::npos) return out;
        mission_id = static_cast<std::uint32_t>(std::strtoul(tpl.substr(1, e - 1).c_str(), nullptr, 10));
        start = 0;
    }

    const auto field = [&](const char* k) {
        std::string key(1, '\x01');
        key += k;
        key += '\0';
        return field_after(tpl, start, key);
    };
    const std::string gamemode = field("gamemode");
    const std::string map = field("map");
    const std::string expr = field("expression");

    out.valid = true;
    out.target = list_max(field("points"));
    out.xp = list_sum(field("xp_expr"));
    // Progress counts only when the account's mission SO is this exact mission for the
    // active period; otherwise nothing has been earned this week yet.
    out.progress = (recurring_mission_.valid && recurring_mission_.mission_id == mission_id &&
                    recurring_mission_.period == current_period_)
                       ? recurring_mission_.progress
                       : 0;

    std::string name;
    if (!gamemode.empty()) name = title_case(gamemode);
    if (!map.empty()) name += (name.empty() ? "" : " - ") + map_label(map);
    if (!expr.empty()) name += (name.empty() ? "" : " - ") + action_label(expr);
    out.name = std::move(name);
    return out;
}

void GcSession::on_unknown_so(int type_id, const std::string& data) {
    // Discovery aid: hex-dump small unmodeled SO blobs (logged once per type) so their
    // protobuf fields can be decoded by hand -- the recurring-mission SO type id is
    // undocumented and shape-matching alone is ambiguous (several SO types carry the
    // account id in field 1).
    if (logged_so_types_.insert(type_id).second)
        SAM_LOG_DEBUG("gc: unhandled SO type_id={} ({} bytes) hex={}", type_id, data.size(),
                      hex_dump(data));
    // Recurring (weekly) mission: a CSOAccountRecurringMission for THIS account with all of
    // mission_id/period/progress present. Requiring progress rules out look-alike SO types
    // whose field 1 merely happens to equal the account id (e.g. type 3, mission_id=2).
    CSOAccountRecurringMission rm;
    if (rm.ParseFromString(data) && rm.has_account_id() &&
        rm.account_id() == static_cast<std::uint32_t>(cm_.steam_id() & 0xffffffffULL) &&
        rm.has_mission_id() && rm.has_period() && rm.has_progress() && rm.mission_id() != 0) {
        // An account can hold mission SOs from several periods; keep the most recent so a
        // stale older-week mission doesn't mask the current one.
        if (!recurring_mission_.valid || rm.period() >= recurring_mission_.period) {
            recurring_mission_.valid = true;
            recurring_mission_.mission_id = rm.mission_id();
            recurring_mission_.period = rm.period();
            recurring_mission_.progress = rm.progress();
        }
        if (recurring_so_type_ != type_id) {
            recurring_so_type_ = type_id;
            SAM_LOG_DEBUG("gc: recurring mission SO type_id={} mission_id={} period={} progress={}",
                          type_id, rm.mission_id(), rm.period(), rm.progress());
        }
        ++so_seq_;
    }
}

bool GcSession::send_gc(std::uint32_t gc_emsg, const google::protobuf::MessageLite& body) {
    CMsgGCClient gc;
    build_gc_client(gc, gc_emsg, body.SerializeAsString());
    return cm_.send(steam_cm::EMsg::ClientToGC, gc);
}

bool GcSession::pump(int timeout_ms) {
    if (abort_ && abort_->load(std::memory_order_relaxed)) return false;
    return cm_.pump(timeout_ms, [this](std::uint32_t e, const ::CMsgProtoBufHeader& h,
                                       const std::string& b) { on_cm_message(e, h, b); });
}

void GcSession::pump_for(int total_ms) {
    const auto deadline = clock::now() + std::chrono::milliseconds(total_ms);
    while (clock::now() < deadline)
        if (!pump(500)) return;
}

bool GcSession::wait_until(const std::function<bool()>& pred, int timeout_ms) {
    if (pred()) return true;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    while (clock::now() < deadline) {
        if (!pump(500)) return pred();
        if (pred()) return true;
    }
    return pred();
}

bool GcSession::pop_notification(GcNotification& out) {
    if (notifications_.empty()) return false;
    out = std::move(notifications_.front());
    notifications_.pop_front();
    return true;
}

void GcSession::on_cm_message(std::uint32_t emsg, const ::CMsgProtoBufHeader&,
                              const std::string& body) {
    if (emsg != steam_cm::EMsg::ClientFromGC) return;
    CMsgGCClient gc;
    if (!gc.ParseFromString(body)) return;
    std::uint32_t gc_emsg = 0;
    std::string gc_body;
    if (parse_gc_client(gc, gc_emsg, gc_body)) on_gc_message(gc_emsg, gc_body);
}

void GcSession::upsert_econ_item(const std::string& data) {
    CSOEconItem it;
    if (it.ParseFromString(data)) {
        inventory_[it.id()] = parse_econ_item(it);
        ++so_seq_;
    }
}

void GcSession::erase_econ_item(const std::string& data) {
    CSOEconItem it;
    if (!it.ParseFromString(data)) return;
    if (inventory_.erase(it.id()) != 0) ++so_seq_;
}

void GcSession::load_personal_store(const std::string& data) {
    CSOAccountItemPersonalStore ps;
    if (!ps.ParseFromString(data)) return;
    personal_store_.loaded = true;
    personal_store_.generation_time = ps.generation_time();
    personal_store_.redeemable_balance = ps.redeemable_balance();
    personal_store_.items.assign(ps.items().begin(), ps.items().end());
    ++so_seq_;
}

void GcSession::on_gc_message(std::uint32_t gc_emsg, const std::string& body) {
    switch (gc_emsg) {
        case GcMsg::ClientWelcome: {
            CMsgClientWelcome w;
            if (!w.ParseFromString(body)) return;
            for (const auto& cache : w.outofdate_subscribed_caches())
                for (const auto& sub : cache.objects()) {
                    if (sub.type_id() == kSoTypeEconItem)
                        for (const auto& d : sub.object_data()) upsert_econ_item(d);
                    else if (sub.type_id() == kSoTypePersonalStore) {
                        if (sub.object_data_size() > 0) load_personal_store(sub.object_data(0));
                    } else
                        for (const auto& d : sub.object_data()) on_unknown_so(sub.type_id(), d);
                }
            welcome_received_ = true;
            SAM_LOG_INFO("gc: welcome received, {} items in cache", inventory_.size());
            return;
        }
        case GcMsg::SOCacheSubscribed: {
            CMsgSOCacheSubscribed s;
            if (!s.ParseFromString(body)) return;
            for (const auto& sub : s.objects()) {
                if (sub.type_id() == kSoTypeEconItem)
                    for (const auto& d : sub.object_data()) upsert_econ_item(d);
                else if (sub.type_id() == kSoTypePersonalStore) {
                    if (sub.object_data_size() > 0) load_personal_store(sub.object_data(0));
                } else
                    for (const auto& d : sub.object_data()) on_unknown_so(sub.type_id(), d);
            }
            return;
        }
        case GcMsg::SOCreate:
        case GcMsg::SOUpdate: {
            CMsgSOSingleObject so;
            if (!so.ParseFromString(body)) return;
            if (so.type_id() == kSoTypeEconItem) upsert_econ_item(so.object_data());
            else if (so.type_id() == kSoTypePersonalStore) load_personal_store(so.object_data());
            else on_unknown_so(so.type_id(), so.object_data());
            return;
        }
        case GcMsg::SODestroy: {
            CMsgSOSingleObject so;
            if (!so.ParseFromString(body)) return;
            if (so.type_id() == kSoTypeEconItem) erase_econ_item(so.object_data());
            return;
        }
        case GcMsg::SOUpdateMultiple: {
            CMsgSOMultipleObjects mo;
            if (!mo.ParseFromString(body)) return;
            for (const auto& obj : mo.objects_modified()) {
                if (obj.type_id() == kSoTypeEconItem) upsert_econ_item(obj.object_data());
                else if (obj.type_id() == kSoTypePersonalStore) load_personal_store(obj.object_data());
                else on_unknown_so(obj.type_id(), obj.object_data());
            }
            return;
        }
        case GcMsg::ItemCustomizationNotification: {
            CMsgGCItemCustomizationNotification n;
            if (!n.ParseFromString(body)) return;
            GcNotification note;
            note.request = n.request();
            note.item_ids.assign(n.item_id().begin(), n.item_id().end());
            notifications_.push_back(std::move(note));
            return;
        }
        case GcMsg::UpdateItemSchema: {
            CMsgUpdateItemSchema s;
            if (!s.ParseFromString(body)) return;
            if (!s.items_game_url().empty()) items_game_url_ = s.items_game_url();
            item_schema_version_ = s.item_schema_version();
            return;
        }
        case GcMsg::PlayersProfile: {
            CMsgGCCStrike15_v2_PlayersProfile p;
            if (!p.ParseFromString(body)) return;
            // A reply carries one profile per requested account; route each by its own
            // account_id (batch requests interleave, so order isn't reliable).
            const auto own = static_cast<std::uint32_t>(cm_.steam_id() & 0xffffffffULL);
            for (const auto& a : p.account_profiles()) {
                ProfileResult r;
                r.account_id = a.account_id();
                r.level = a.player_level();
                r.cur_xp = a.player_cur_xp();
                r.bonus_flags = a.player_xp_bonus_flags();
                if (a.has_medals()) {
                    r.featured_medal_defidx = a.medals().featured_display_item_defidx();
                    r.medal_defidx.assign(a.medals().display_items_defidx().begin(),
                                          a.medals().display_items_defidx().end());
                }
                // Competitive standing. `rankings` carries one entry per mode keyed by
                // rank_type_id (11 = Premier CS Rating, 7 = Wingman); verified live.
                for (const auto& rk : a.rankings()) {
                    if (rk.rank_type_id() == 11) {
                        r.premier_rating = static_cast<int>(rk.rank_id());
                        r.premier_wins = static_cast<int>(rk.wins());
                    } else if (rk.rank_type_id() == 7) {
                        r.wingman_rank = static_cast<int>(rk.rank_id());
                        r.wingman_wins = static_cast<int>(rk.wins());
                    }
                }
                // The session's own profile also feeds player_xp() -- launch()'s wait and the
                // live screen read it -- and bumps so_seq_ to re-post a snapshot. Foreign
                // profiles only queue, so a puller never churns snapshots for other accounts.
                if (r.account_id == own) {
                    player_xp_.level = r.level;
                    player_xp_.cur_xp = r.cur_xp;
                    player_xp_.bonus_flags = r.bonus_flags;
                    player_xp_.featured_medal_defidx = r.featured_medal_defidx;
                    player_xp_.medal_defidx = r.medal_defidx;
                    player_xp_.valid = true;
                    ++so_seq_;
                }
                profile_results_.push_back(std::move(r));
            }
            return;
        }
        case GcMsg::RecurringMissionSchema: {
            CMsgRecurringMissionSchema s;
            if (!s.ParseFromString(body)) return;
            // Keep the template blob for the period that contains "now" -- the active week,
            // whose template defines the current mission's name/target/xp.
            const auto now = static_cast<std::uint32_t>(std::time(nullptr));
            constexpr std::uint32_t kWeek = 7 * 24 * 3600;
            for (const auto& m : s.missions())
                if (m.period() <= now && now < m.period() + kWeek &&
                    m.mission_templates_size() > 0) {
                    current_period_ = m.period();
                    current_period_templates_ = m.mission_templates(0);
                    ++so_seq_;  // a fresh template may complete the mission card
                }
            SAM_LOG_INFO("gc: recurring mission schema, active period={} ({} bytes)",
                         current_period_, current_period_templates_.size());
            return;
        }
        default:
            return;
    }
}

}  // namespace sam::cs2_gc
