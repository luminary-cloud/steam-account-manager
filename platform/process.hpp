#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sam::platform::process {

// Returns the PIDs of all running processes with the given image name (case-insensitive).
std::vector<std::uint32_t> find_by_image_name(std::wstring_view name);

bool terminate(std::uint32_t pid);

// Launches `exe` with `args`. Returns the PID of the new process, or std::nullopt
// on failure. `working_dir` defaults to the directory of `exe`.
std::optional<std::uint32_t> launch(const std::filesystem::path& exe,
                                     const std::wstring& args,
                                     const std::optional<std::filesystem::path>& working_dir = {});

// Waits for the process to exit. Returns false on timeout.
bool wait_for_exit(std::uint32_t pid, std::chrono::milliseconds timeout);

}  // namespace sam::platform::process
