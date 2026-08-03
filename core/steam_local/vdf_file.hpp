#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/steam_local/loginusers.hpp"  // VdfNode

namespace sam::steam_local {

// File-level helpers for editing Steam's own text VDFs in place. Steam's casing for keys
// varies across versions, so lookups here are case-insensitive but create with whatever
// casing the caller passes.

// Whole file as a string; empty when the file is missing or unreadable.
std::string read_text_file(const std::filesystem::path& path);

// Steam wraps each VDF in a single top-level store, so every root child is written at
// depth 0 to match its layout.
std::string serialize_root(const VdfNode& root);

// Existing block child, or nullptr.
VdfNode* find_block_ci(VdfNode& parent, std::string_view key);

// Existing scalar child, or nullptr.
VdfNode* find_scalar_ci(VdfNode& parent, std::string_view key);

// Existing block child, appended (with `key`'s casing) if absent.
VdfNode& find_or_add_block(VdfNode& parent, std::string_view key);

// Overwrites the scalar child if present, appends it otherwise.
void upsert_scalar_ci(VdfNode& parent, std::string_view key, std::string_view value);

// Backs `path` up with a UTC timestamp suffix, then writes `text` atomically.
//
// restrict_acl is passed through to platform::atomic_write_file: false for files Steam's
// sandboxed helper must still read. lock_read_only re-applies the read-only attribute
// afterwards, so Steam's shutdown rewrite can't flip our value back; it only takes effect
// when the file already existed, since locking a brand-new file would block Steam's own
// first-login setup writes. A prior lock is cleared first so the backup and replace can
// proceed. When the file doesn't exist there is nothing to back up, so the parent tree is
// created and `text` written straight out.
//
// False on failure, with `error` filled.
bool backup_and_write(const std::filesystem::path& path, const std::string& text,
                      bool restrict_acl, bool lock_read_only, std::string& error);

}  // namespace sam::steam_local
