#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sam::platform {

// Atomic: streams to path.tmp, FlushFileBuffers, MoveFileEx REPLACE_EXISTING +
// WRITE_THROUGH.
void atomic_write_file(const std::filesystem::path& path,
                       std::span<const std::uint8_t> data);

// Restricts the DACL on `path` to the current user SID only. Best-effort.
bool restrict_to_current_user(const std::filesystem::path& path);

// Throws std::runtime_error on failure.
std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path);

}  // namespace sam::platform
