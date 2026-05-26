#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sam::platform {

// Writes `data` to `path` atomically: streams to `path.tmp`, FlushFileBuffers,
// MoveFileEx with REPLACE_EXISTING + WRITE_THROUGH.
void atomic_write_file(const std::filesystem::path& path,
                       std::span<const std::uint8_t> data);

// Restricts the DACL on `path` to the current user SID only. Best-effort:
// returns true if the new ACL was applied, false on any failure.
bool restrict_to_current_user(const std::filesystem::path& path);

// Reads a binary file into a vector. Throws std::runtime_error on failure.
std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path);

}  // namespace sam::platform
