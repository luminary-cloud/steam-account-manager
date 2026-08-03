#include "app/cleaner_runner.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "app/job_pump.hpp"
#include "core/cleaner/profiles.hpp"
#include "core/cleaner/steam_scan.hpp"
#include "core/launch/first_login_reapply.hpp"
#include "core/launch/steam_launcher.hpp"
#include "core/log.hpp"

namespace sam::app::cleaner_runner {
namespace {

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const char* trigger_name(Trigger why) {
    switch (why) {
        case Trigger::Manual: return "manual";
        case Trigger::BeforeLaunch: return "before-launch";
        case Trigger::Unlock: return "unlock";
        case Trigger::Exit: return "exit";
    }
    return "?";
}

const cleaner::Profile* resolve_profile(CleanerProfile p) {
    const auto profiles = cleaner::built_in_profiles();
    const auto idx = static_cast<std::size_t>(p);
    return idx < profiles.size() ? &profiles[idx] : nullptr;
}

cleaner::PreserveList make_preserve_list(const Settings& s) {
    cleaner::PreserveList list;
    list.account_ids.reserve(s.cleaner.preserved_steam_ids.size());
    for (const std::uint64_t id : s.cleaner.preserved_steam_ids) {
        if (id != 0) list.account_ids.push_back(std::to_wstring(id));
    }
    return list;
}

bool close_steam() {
    launch::LaunchResult out;
    auto exe = launch::resolve_steam_exe(out);
    if (!exe) {
        SAM_LOG_WARN("cleaner: {}", out.message);
        return false;
    }
    if (!launch::shutdown_running_steam(*exe, out)) {
        SAM_LOG_WARN("cleaner: {}", out.message);
        return false;
    }
    return true;
}

}  // namespace

std::optional<cleaner::Plan> build(const Settings& s, bool measure) {
    const auto* profile = resolve_profile(s.cleaner.profile);
    if (profile == nullptr) return std::nullopt;

    auto install = cleaner::discover_install();
    if (!install) return std::nullopt;

    const auto accounts = cleaner::enumerate_accounts(*install);
    const auto libraries = cleaner::discover_libraries(*install);
    const cleaner::ResolveContext ctx{*install, accounts, libraries};

    const auto preserve = make_preserve_list(s);
    cleaner::PlanOptions opts;
    opts.preserve = &preserve;
    opts.measure = measure;
    return cleaner::build_plan_by_ids(profile->target_ids, ctx, opts);
}

std::optional<cleaner::CleanResult> run_blocking(const Settings& s, bool steam_already_down,
                                                  bool measure) {

    if (!s.cleaner.enabled || s.safe_mode) return std::nullopt;

    if (!steam_already_down && launch::first_login_reapply::in_flight()) {
        SAM_LOG_INFO("cleaner: skipped, a first-login reapply is in flight");
        return std::nullopt;
    }

    auto plan = build(s, measure);
    if (!plan) {
        SAM_LOG_WARN("cleaner: no plan (Steam not installed?)");
        return std::nullopt;
    }

    if (!steam_already_down && !close_steam()) return std::nullopt;

    const auto result = cleaner::execute(*plan, {});
    SAM_LOG_INFO("cleaner: {} steps ok, {} failed, {} bytes", result.succeeded, result.failed,
                 result.bytes_freed);
    return result;
}

void run_async(AppState& state, Trigger why) {

    if (state.settings.safe_mode) {
        SAM_LOG_INFO("cleaner: {} run skipped, safe mode is on", trigger_name(why));
        return;
    }

    bool expected = false;
    if (!state.cleaner_busy.compare_exchange_strong(expected, true)) {
        SAM_LOG_INFO("cleaner: {} run skipped, one is already running", trigger_name(why));
        return;
    }

    const Settings snapshot = state.settings;
    const bool measure = (why == Trigger::Manual);
    const int toast_seconds = state.settings.notifications.toast_duration_seconds;
    const char* tag = trigger_name(why);
    SAM_LOG_INFO("cleaner: {} run starting", tag);

    job_pump::submit([&state, snapshot, measure, toast_seconds, tag] {
        auto result = run_blocking(snapshot, false, measure);
        state.post_ui_callback([&state, result = std::move(result), toast_seconds, tag] {
            state.cleaner_busy.store(false);
            if (!result) return;
            state.cleaner_last = result;

            state.cleaner_preview.reset();

            ui::widgets::ToastItem t;
            t.id = "cleaner";
            t.message = "Cleaner (" + std::string{tag} + "): " +
                        std::to_string(result->succeeded) + " removed";
            if (result->failed > 0) {
                t.message += ", " + std::to_string(result->failed) + " failed";
                t.is_warning = true;
            }
            t.expires_at_unix = now_unix() + toast_seconds;
            state.toasts.push(std::move(t));
        });
    });
}

}  // namespace sam::app::cleaner_runner
