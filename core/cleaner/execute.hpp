#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/cleaner/plan.hpp"

namespace sam::cleaner {

struct CleanResult {
    int succeeded = 0;
    int failed = 0;
    std::uint64_t bytes_freed = 0;  // 0 unless the plan was built with PlanOptions::measure
    std::vector<std::wstring> failure_messages;
};

struct CleanOptions {
    std::function<void(int done, int total)> on_progress;  // null = no callback
};

// Runs every step of the plan in order. There is no backup and no undo: the plan is the
// contract, so preview it before running one that touches account residue. Steam must already
// be shut down: it rewrites loginusers.vdf / config.vdf / local.vdf from memory when it exits
// and would put back whatever was removed here.
CleanResult execute(const Plan& plan, const CleanOptions& opts);

}  // namespace sam::cleaner
