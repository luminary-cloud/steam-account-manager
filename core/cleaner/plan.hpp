#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/cleaner/operation.hpp"
#include "core/cleaner/preserve.hpp"
#include "core/cleaner/steam_scan.hpp"
#include "core/cleaner/targets.hpp"

namespace sam::cleaner {

struct PlanStep {
    std::string target_id;
    Operation op;
};

struct Plan {
    std::vector<PlanStep> steps;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_file_count = 0;
};

struct PlanOptions {
    const PreserveList* preserve = nullptr;  // applied at plan-build time

    // Sizing walks every tree recursively, which on a shadercache is by far the slowest part of
    // planning and buys nothing for a run whose numbers nobody reads. The preview turns it on;
    // the automatic triggers leave it off and report succeeded/failed counts only.
    bool measure = true;
};

// Resolves the targets, applies the preserve list, and returns a self-contained Plan the
// executor can run without consulting the preserve list again.
Plan build_plan(std::span<const Target* const> targets, const ResolveContext& ctx,
                const PlanOptions& opts);

// Convenience: looks the ids up in the catalog first, skipping any that don't resolve.
Plan build_plan_by_ids(std::span<const std::string> target_ids, const ResolveContext& ctx,
                       const PlanOptions& opts);

struct AutoLoginRedirect {
    std::wstring account_name;  // value to write into HKCU\...\Steam\AutoLoginUser
    std::wstring steamid64;     // matching loginusers.vdf entry, used to flip mostrecent
};

// Picks the preserved account that should take over AutoLoginUser when the current auto-login
// account is being wiped. Prefers most_recent, else the first preserved account with a login
// name. nullopt when none qualifies.
std::optional<AutoLoginRedirect> pick_autologin_redirect(std::span<const AccountInfo> accounts,
                                                          const PreserveList& preserve);

}  // namespace sam::cleaner
