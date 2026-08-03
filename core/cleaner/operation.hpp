#pragma once

#include <cstdint>
#include <string>

namespace sam::cleaner {

// Values use `Remove` instead of `Delete` because Win32 defines DeleteFile and RemoveDirectory
// as macros that would rewrite the enum names at preprocessing time.
enum class OpKind {
    RemoveFile,
    RemoveTree,          // recursive directory delete
    RemoveRegistryValue,
    RemoveRegistryKey,
    VdfRemoveChild,      // target = vdf path, value_name = backslash-separated key path to remove
    VdfSetValue,         // target = vdf path, value_name = backslash-separated key path,
                         // payload = string value. Flips mostrecent/Timestamp on the redirect
                         // target.
    WriteRegistryString, // writes `payload` to the value; used to redirect AutoLoginUser to a
                         // preserved account when the previous one is wiped
};

// A single concrete cleanup action. Targets resolve to one or more of these.
struct Operation {
    OpKind kind = OpKind::RemoveFile;

    // Filesystem path for RemoveFile / RemoveTree.
    // Registry ops: full path including hive prefix, e.g. "HKCU\\Software\\Valve\\Steam".
    std::wstring target;

    // Value name for registry value ops, key path for the VDF ops. Empty otherwise.
    std::wstring value_name;

    // SteamID64 of the account this op belongs to, or empty if global. The preserve list uses
    // this to spare account-scoped ops.
    std::wstring account_steamid64;

    // Filled by the planner when PlanOptions::measure is on; 0 otherwise.
    std::uint64_t size_bytes = 0;

    // Replacement value for WriteRegistryString / VdfSetValue. Empty for other kinds.
    std::wstring payload;
};

}  // namespace sam::cleaner
