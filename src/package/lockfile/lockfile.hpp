// Cova — lockfiles (PRD §15).
//
// Two lockfiles with distinct responsibilities:
//   cova.lock            — dependency source resolution results
//   cova-toolchain.lock  — toolchain, sysroot, SDK and runtime identities
//
// Lockfile writes are atomic (temp + rename) so an interrupted run can never
// leave a truncated lock (PRD §19.2). Locked mode is enforced by the resolvers.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "core/toml/toml.hpp"
#include "platform/abi/abi.hpp"
#include "platform/triple/triple.hpp"
#include "toolchain/descriptor.hpp"

namespace cova::lockfile {

namespace stdfs = std::filesystem;

// One resolved dependency source.
struct LockedPackage {
  std::string name;
  std::string version;
  // Exactly one source form is recorded per entry.
  std::optional<std::string> source_url;   // git or registry URL
  std::optional<std::string> git_rev;      // pinned commit
  std::optional<std::string> registry;
  std::optional<std::string> path;         // workspace/path dependency
  std::string source_hash;                 // sha256 of the fetched tree
  std::vector<std::string> checksum;       // per-file hashes when vendored
  std::vector<std::string> features;
  std::vector<std::string> dependencies;   // names only
  bool dev = false;

  std::string identity() const;            // name + version + source
  std::string to_toml() const;
};

struct DependencyLock {
  std::string version = "1";               // lockfile format version
  std::string generated_by;                // "cova 0.1.0"
  std::vector<LockedPackage> packages;
  // Hash of the manifest inputs that produced this lock (drift detection).
  std::string manifest_fingerprint;

  const LockedPackage* find(std::string_view name) const;
  std::string to_toml() const;
  static Result<DependencyLock> from_toml(std::string_view text);
  static Result<DependencyLock> load(const stdfs::path& path);
  Status save(const stdfs::path& path) const;
};

// One locked toolchain + sysroot identity. PRD §15.2 requires the *identity*,
// not just a version: two builds of the same version are not interchangeable.
struct LockedToolchain {
  std::string target;                       // canonical triple
  std::string toolchain_id;
  std::string toolchain_family;
  std::string toolchain_version;
  std::string content_hash;                 // managed components: content identity
  std::string source;                       // "system" | "managed" | "registry"
  std::string host;                         // host triple
  // System toolchains get a verifiable fingerprint rather than a content hash.
  std::string system_fingerprint;
  std::string sysroot_id;
  std::string sysroot_hash;
  // ABI facts that must match for artifact reuse.
  std::string abi_key;
  std::vector<std::string> components;      // "role=/path" entries
  std::string to_toml() const;
};

struct ToolchainLock {
  std::string version = "1";
  std::string generated_by;
  std::vector<LockedToolchain> entries;
  const LockedToolchain* for_target(std::string_view target) const;
  std::string to_toml() const;
  static Result<ToolchainLock> from_toml(std::string_view text);
  static Result<ToolchainLock> load(const stdfs::path& path);
  Status save(const stdfs::path& path) const;
};

// Builds a lock entry from a resolved toolchain (called by the resolver path).
LockedToolchain lock_entry_from(const toolchain::ResolvedToolchain& resolved);
// Verifies a resolved toolchain satisfies an existing lock entry.
struct LockMismatch {
  bool matches = true;
  std::vector<std::string> differences;
};
LockMismatch check_against_lock(const toolchain::ResolvedToolchain& resolved,
                                const LockedToolchain& locked);

// Whether a lockfile needs rewriting after a resolution.
bool needs_rewrite(const ToolchainLock& lock, const toolchain::ResolvedToolchain& resolved,
                   bool locked_mode);

// Fingerprints the manifest inputs that affect dependency resolution.
std::string manifest_fingerprint(std::string_view manifest_text,
                                 const std::vector<std::string>& member_fingerprints);

}  // namespace cova::lockfile
