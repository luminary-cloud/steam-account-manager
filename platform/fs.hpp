#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sam::platform {

// Atomic: streams to path.tmp, FlushFileBuffers, MoveFileEx REPLACE_EXISTING +
// WRITE_THROUGH.
//
// restrict_acl=true (default) locks the file's DACL to the current user, for our
// own secrets (vault, master-password cache). Pass false for files that Steam
// itself must read/rewrite (its VDFs): locking them strips the inherited ACEs --
// including ALL [RESTRICTED] APPLICATION PACKAGES -- that Steam's sandboxed
// steamwebhelper needs, so it can't read the injected login token and the client
// bounces back to the account picker.
void atomic_write_file(const std::filesystem::path& path,
                       std::span<const std::uint8_t> data,
                       bool restrict_acl = true);

// Restricts the DACL on `path` to the current user SID only. Best-effort.
bool restrict_to_current_user(const std::filesystem::path& path);

// Sets or clears the read-only attribute (FILE_ATTRIBUTE_READONLY) on `path`.
// Used to stop Steam rewriting a config file we pre-set (it can read a read-only
// file but its shutdown rewrite fails, so our value sticks). Best-effort: returns
// false if the file is missing or the attribute change fails. Note an atomic
// replace can't overwrite a read-only file, so clear it before re-writing.
bool set_file_read_only(const std::filesystem::path& path, bool read_only);

// Throws std::runtime_error on failure.
std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path);

}  // namespace sam::platform
