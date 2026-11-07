// Cova — workspace model (PRD §14).
//
// A workspace is a root cova.toml with [workspace] members. Members may be
// packages or glob patterns. The workspace owns one shared lockfile and one
// shared toolchain policy.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "manifest/manifest.hpp"
#include "platform/triple/triple.hpp"

namespace cova::workspace {

namespace stdfs = std::filesystem;

struct MemberEntry {
  stdfs::path root;
  std::string name;
  manifest::PackageKind kind = manifest::PackageKind::Executable;
  // Non-fatal problems found while loading this member.
  std::vector<std::string> warnings;
};

struct Workspace {
  manifest::Manifest root;                 // the root cova.toml
  std::vector<MemberEntry> members;
  stdfs::path root_dir;
  // Where cova.lock / cova-toolchain.lock live.
  stdfs::path lockfile;
  stdfs::path toolchain_lockfile;

  bool empty() const { return members.empty(); }
  const MemberEntry* find_member(std::string_view name) const;
  // Effective toolchain policy: member overrides win, then workspace, then default.
  manifest::ToolchainSection toolchain_policy_for(const MemberEntry& member) const;
  manifest::FormatSection format_policy_for(const MemberEntry& member) const;
  // Member directories as workspace-relative strings, sorted.
  std::vector<std::string> member_relpaths() const;
};

struct LoadOptions {
  // Discover members by glob; when false only literal members are used.
  bool expand_globs = true;
  bool recursive = true;
};

// Loads the workspace rooted at `root_dir` (which must contain cova.toml).
Result<Workspace> load(const stdfs::path& root_dir, const LoadOptions& options = {});

// Expands member patterns ("packages/*") relative to the workspace root.
Result<std::vector<stdfs::path>> expand_members(const stdfs::path& root, const manifest::Manifest& root_manifest,
                                                const LoadOptions& options);

// Searches upward for a workspace root. Falls back to the package manifest.
struct ProjectContext {
  stdfs::path current_dir;
  stdfs::path manifest_path;
  stdfs::path root_dir;
  bool is_workspace = false;
  Workspace workspace;
  // The selected member when running inside a workspace.
  std::optional<MemberEntry> current_member;
};

Result<ProjectContext> discover_project(const stdfs::path& start_dir);

// Chooses which members a command acts on (--package, -p, --workspace).
struct SelectionOptions {
  std::vector<std::string> packages;
  bool workspace_wide = false;
  // When neither is given, the member containing the cwd (or all for the root).
  stdfs::path current_dir;
};
Result<std::vector<const MemberEntry*>> select_members(const Workspace& workspace,
                                                      const SelectionOptions& options);

// Validates member uniqueness and dependency cycles between members.
Result<std::vector<std::string>> validate_workspace(const Workspace& workspace);

// Topologically orders members by intra-workspace dependencies.
Result<std::vector<std::string>> member_build_order(const Workspace& workspace);

}  // namespace cova::workspace
