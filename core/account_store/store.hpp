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

// Returns true if a vault file is present at `path` and at least has the magic bytes.
bool vault_exists(const std::filesystem::path& path);

// Creates a new empty vault file at `path` encrypted with `password`.
// Throws on I/O failure.
void create_new_vault(const std::filesystem::path& path,
                      const crypto::SecureString& password);

// Loads the vault from disk. Throws WrongPassword if the GCM tag does not verify,
// CorruptVault if the file is structurally broken, or UnsupportedVersion if the
// schema_version is newer than we recognise.
Vault load_vault(const std::filesystem::path& path,
                 const crypto::SecureString& password);

// Writes the vault back atomically (write to .tmp, fsync, rename).
void save_vault(const std::filesystem::path& path,
                const Vault& vault,
                const crypto::SecureString& password);

// A "bundle" is a vault file written under a separate passphrase intended for
// portability across machines. The on-disk format is identical to the regular
// vault, so save_bundle / load_bundle simply forward to save_vault / load_vault
// under different names so call sites are self-documenting.
void save_bundle(const std::filesystem::path& path,
                 const Vault& bundle,
                 const crypto::SecureString& passphrase);

Vault load_bundle(const std::filesystem::path& path,
                  const crypto::SecureString& passphrase);

// Returns a new Vault containing only the accounts whose id is in `ids`, plus
// the Tag entries referenced by those accounts' tag_ids (so the bundle is
// self-contained on the receiving side).
Vault subset_vault(const Vault& source, std::span<const std::string> ids);

struct MergeReport {
    std::size_t added = 0;
    std::size_t overwritten = 0;
    std::vector<std::string> overwritten_logins;
    std::size_t tags_added = 0;
};

// Merges `imported` into `dst`. Match policy for accounts: steam_id_64 when
// non-zero on both sides, otherwise ASCII case-insensitive `login`. When a
// match is found the destination account is overwritten in place but its
// existing ULID is preserved so other references stay valid. Tags are deduped
// by id; existing tag entries are left untouched (a rename in dst wins).
MergeReport merge_into(Vault& dst, Vault imported);

// Non-mutating dry run: computes what merge_into() would do, so the UI can
// show a confirmation summary before the user commits.
MergeReport preview_merge(const Vault& dst, const Vault& imported);

// Finds an existing account in `vault` that should be considered the same as
// an incoming account identified by (steam_id_64, login). Matches by
// steam_id_64 first when non-zero, otherwise ASCII case-insensitive login.
// Returns nullptr if no match. Same policy as merge_into().
Account* find_existing_account(Vault& vault,
                               std::uint64_t steam_id_64,
                               std::string_view login);

// Returns the id of the reserved "NFA" group, creating it in `vault` if it isn't
// there yet. Used by the JWT-token import path to bucket Non-Full-Access
// accounts. Idempotent across repeated imports.
std::string ensure_nfa_group(Vault& vault);

}  // namespace sam::core::store
