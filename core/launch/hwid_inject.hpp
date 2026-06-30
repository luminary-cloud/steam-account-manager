#pragma once

#include <cstdint>
#include <string>

#include "core/account_store/account.hpp"

namespace sam::launch {

enum class InjectOutcome { Skipped, Success, Failed };

struct InjectResult {
    InjectOutcome outcome = InjectOutcome::Skipped;
    std::string error;
};

InjectResult maybe_inject_hwid(const core::Account& account, std::uint32_t steam_pid,
                                std::uint32_t component_mask = 0x3FFu);

}  // namespace sam::launch
