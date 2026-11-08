// Cova — local dependency resolution (PRD §10, §15).
//
// This milestone can resolve exactly one dependency source: a local path.
// Registry and git sources have no verified fetcher yet, so they are rejected
// rather than silently ignored (PRD §18.1, rule 5). Resolution is deterministic:
// packages are visited dependency-first and every serialised form is sorted.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "manifest/manifest.hpp"
#include "package/lockfile/lockfile.hpp"
#include "platform/triple/triple.hpp"

namespace cova::package {

namespace stdfs = std::filesystem;

// One package in the resolved local graph.
struct ResolvedPackage {
  manifest::Manifest manifest;
  // Local dependencies built for the requested target, dependency-first.
  // Names refer to other ResolvedPackage entries.
  std::vector<std::string> build_dependencies;
  // Every local dependency declaration, regardless of target filters. This is
  // what a dependency lockfile records, so locking stays stable when the same
  // tree is built for a different target.
  std::vector<std::string> all_dependencies;
  // True when this package is one of the packages the command acts on, rather
  // than one reached through a dependency edge.
  bool root = false;
  // Deterministic text describing the resolution inputs of this package.
  std::string resolution_input;
};

struct DependencyGraph {
  // Dependency-first: every package appears after the packages it needs.
  std::vector<ResolvedPackage> packages;
  // Hash over every package's resolution inputs.
  std::string fingerprint;

  const ResolvedPackage* find(std::string_view name) const;
};

struct ResolveRequest {
  platform::Triple target;
  // Workspace members, if the project is part of one. They are used to resolve
  // name-only dependencies and path dependencies that point at a member. The
  // members are not automatically built; only roots and their dependencies are.
  std::vector<manifest::Manifest> workspace_members;
  // Packages the command acts on.
  std::vector<manifest::Manifest> roots;
  // Directory that will contain `cova.lock`; used to make recorded paths
  // relative to the project instead of absolute to this machine.
  stdfs::path lock_root;
  // Development dependencies are locked either way, but are only built and
  // linked when a test build asks for them. A remote development dependency is
  // skipped for a regular build because it is not needed there.
  bool include_dev = true;
};

// Resolves roots and their transitive local path dependencies.
Result<DependencyGraph> resolve(const ResolveRequest& request);

// Applies a resolved graph to `cova.lock`.
//
// In unlocked mode a missing or stale lockfile is rewritten atomically. In
// locked mode a missing, stale or would-change lockfile is an error and nothing
// is written (PRD §15.2).
struct LockOutcome {
  lockfile::DependencyLock lock;
  bool present = false;
  bool up_to_date = false;
  bool wrote = false;
  // Human-readable reasons the lockfile did not match, for locked-mode errors.
  std::vector<std::string> differences;
};

Result<LockOutcome> enforce_lock(const DependencyGraph& graph, const stdfs::path& lock_path,
                                 bool locked_mode);

// Converts a resolved graph into its lockfile form. `lock_root` is the
// directory holding `cova.lock`; recorded paths are relative to it.
lockfile::DependencyLock to_lock(const DependencyGraph& graph, const stdfs::path& lock_root);

}  // namespace cova::package
