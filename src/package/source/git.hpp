// Cova — git source fetching (PRD §10.3, §10.4).
//
// The FetchContent-equivalent for Cova: a dependency declared with
// `{ git = "...", rev = "..." }` is cloned into a content-addressed cache and
// checked out at a pinned commit. Nothing is fetched into the project tree, and
// nothing is fetched twice.
//
// Determinism rules (PRD §15):
//   * every checkout is identified by a full 40-character commit id;
//   * a tag or branch rev is resolved to a commit once and pinned in the
//     lockfile, so a moved branch never changes an existing lockfile;
//   * the tree is hashed from git's own objects (sorted path, mode and blob id),
//     never from the extracted files. Reading the object graph is what keeps the
//     hash independent of `core.autocrlf`, `.gitattributes` filters, git-lfs and
//     the checkout's line endings, so a lockfile written on one platform stays
//     valid on another.
//
// Cache layout under $COVA_HOME/git:
//   db/<repo-id>/                   bare repository, the fetch cache
//   db/<repo-id>/refs/cova/default  the commit the remote HEAD pointed at, so a
//                                   rev-less dependency resolves offline
//   checkouts/<repo-id>/<commit>/   extracted tree (the package root)
//   tmp/                            staging area for atomic checkout publication
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/common.hpp"

namespace cova::package::git {

namespace stdfs = std::filesystem;

struct FetchRequest {
  std::string url;             // https://, ssh, file://, or a local path
  std::string rev;             // tag, branch, or commit; empty = repository default
  stdfs::path cache_root;      // $COVA_HOME/git; must be absolute
  // Use only what is already cached. Missing sources are an error, never a
  // network request (PRD §4.2 `--offline` / `--frozen`).
  bool offline = false;
  // Re-fetch refs from the origin even when a cached ref would answer. Used by
  // `cova update`, and whenever a dependency has no lock pin to reuse.
  bool refresh = false;
};

struct FetchedSource {
  stdfs::path checkout;        // directory containing the fetched package
  std::string commit;          // pinned 40-character commit id
  std::string tree_hash;       // sha256 over the commit's tree (git objects)
  // True when this call fetched new objects from the remote. A checkout created
  // from already-cached objects does not count.
  bool downloaded = false;
};

// Fetches `request.url` at `request.rev` and returns the checkout holding the
// package. Reuses the cache whenever the pinned commit is already present.
Result<FetchedSource> fetch(const FetchRequest& request);

// True for a full 40-character hexadecimal object id, which can be resolved
// without any ref lookup.
bool is_full_commit(std::string_view rev);

// Removes credentials from a URL before it is recorded in `cova.lock` or shown
// in a diagnostic: `https://user:token@host/x.git` becomes
// `https://***@host/x.git`. The conventional SSH account `git@` is kept
// readable because it carries no secret.
std::string redact_url(std::string_view url);

}  // namespace cova::package::git
