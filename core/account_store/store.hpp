#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/account_store/account.hpp"
#include "core/crypto/secure_string.hpp"

namespace sam::core::store {

struct WrongPassword : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct CorruptVault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct UnsupportedVersion : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// True if a vault file at `path` at least has the magic bytes.
bool vault_exists(const std::filesystem::path& path);

void create_new_vault(const std::filesystem::path& path,
                      const crypto::SecureString& password);

// Throws WrongPassword if the GCM tag does not verify, CorruptVault if the file
// is structurally broken, UnsupportedVersion if schema_version is too new.
Vault load_vault(const std::filesystem::path& path,
                 const crypto::SecureString& password);

// Writes atomically (write to .tmp, fsync, rename).
void save_vault(const std::filesystem::path& path,
                const Vault& vault,
                const crypto::SecureString& password);

// A bundle is a vault written under a separate passphrase for portability; the
// on-disk format is identical, so these just forward to save_vault/load_vault.
void save_bundle(const std::filesystem::path& path,
                 const Vault& bundle,
                 const crypto::SecureString& passphrase);

Vault load_bundle(const std::filesystem::path& path,
                  const crypto::SecureString& passphrase);

// A new Vault with only the accounts whose id is in `ids`, plus the Tag entries
// they reference, so the bundle is self-contained on the receiving side.
Vault subset_vault(const Vault& source, std::span<const std::string> ids);

struct MergeReport {
    std::size_t added = 0;
    std::size_t overwritten = 0;
    std::vector<std::string> overwritten_logins;
    std::size_t tags_added = 0;
};

// Match policy: steam_id_64 when non-zero on both sides, otherwise ASCII
// case-insensitive `login`. A matched dst account is overwritten in place but
// keeps its existing ULID so other references stay valid. Tags are deduped by
// id; existing tag entries are left untouched (a rename in dst wins).
MergeReport merge_into(Vault& dst, Vault imported);

// Non-mutating dry run of merge_into() for a pre-commit confirmation summary.
MergeReport preview_merge(const Vault& dst, const Vault& imported);

// Match policy of merge_into(). Returns nullptr if no match.
Account* find_existing_account(Vault& vault,
                               std::uint64_t steam_id_64,
                               std::string_view login);

// Id of the reserved "NFA" group (Non-Full-Access), creating it if absent.
// Idempotent across repeated imports.
std::string ensure_nfa_group(Vault& vault);

}  // namespace sam::core::store
