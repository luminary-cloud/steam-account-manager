#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sam::platform::process {

std::vector<std::uint32_t> find_by_image_name(std::wstring_view name);

bool terminate(std::uint32_t pid);

std::optional<std::uint32_t> launch(const std::filesystem::path& exe,
                                     const std::wstring& args,
                                     const std::optional<std::filesystem::path>& working_dir = {});

// True if this process runs with an elevated token. The manifest is asInvoker, so this is
// false unless the user consented to a "runas" relaunch (or a scheduled task started us
// at highest privileges).
bool is_elevated();

// Relaunch this exe with `args` through the shell's "runas" verb, so the child prompts for
// elevation and takes over. The caller must exit on success. False if the user declined
// the UAC prompt (ERROR_CANCELLED) or the shell refused, in which case nothing was started.
// There is deliberately no reverse call: an elevated process cannot spawn an unelevated
// child (every CreateProcess/ShellExecute child inherits the elevated token), so dropping
// back to normal means exiting and letting the user launch again.
bool relaunch_elevated(const std::wstring& args);

}  // namespace sam::platform::process
