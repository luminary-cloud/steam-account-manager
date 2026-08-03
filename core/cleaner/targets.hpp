#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/cleaner/operation.hpp"
#include "core/cleaner/steam_scan.hpp"

namespace sam::cleaner {

enum class TargetCategory {
    Cache,
    Log,
    AccountResidue,
    ControllerResidue,
    GameData,
    CrashDump,
};

struct ResolveContext {
    const InstallInfo& install;
    const std::vector<AccountInfo>& accounts;
    const std::vector<std::filesystem::path>& libraries;
};

struct Target {
    std::string id;             // stable, e.g. "steam.htmlcache"
    std::wstring display_name;  // shown in previews
    std::wstring description;
    TargetCategory category;

    // Resolves this target to concrete operations against the given context.
    std::function<std::vector<Operation>(const ResolveContext&)> resolve;
};

// nullptr if not found. There is no UI for picking individual targets: profiles
// (core/cleaner/profiles.hpp) name the ids they bundle.
const Target* find_target(std::string_view id);

}  // namespace sam::cleaner
