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
// restrict_acl=true (default) locks the DACL to the current user, for this app's own secrets.
// Pass false for files Steam itself reads or rewrites: locking them strips the inherited ACEs
// (including ALL [RESTRICTED] APPLICATION PACKAGES) that Steam's sandboxed steamwebhelper
// needs, leaving it unable to read the injected login token.
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

// ---- Bulk delete / measure --------------------------------------------------
// Used by the tracer cleaner, which walks whole Steam cache trees. All of these
// are best-effort and never throw: an unreadable subtree is skipped, not fatal.

// Sum of file sizes under `p`, recursively. 0 if `p` is missing or unreadable.
std::uintmax_t size_recursive(const std::filesystem::path& p);

// Every regular file under `p`, recursively.
std::uintmax_t file_count_recursive(const std::filesystem::path& p);

// DeleteFileW after clearing read-only/hidden/system. On a sharing violation,
// retries via CreateFileW + FILE_FLAG_DELETE_ON_CLOSE. True if the file is gone
// (or never existed).
bool delete_file_forced(const std::filesystem::path& p);

// Walks `p` bottom-up deleting files then directories. True only if everything
// went; a partial delete still removes what it can.
bool delete_directory_recursive(const std::filesystem::path& p);

}  // namespace sam::platform
