// Cova — workspace model (PRD §14).
#include "workspace/workspace.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"

namespace cova::workspace {
namespace {

bool has_glob_magic(std::string_view pattern) {
  return pattern.find('*') != std::string_view::npos ||
         pattern.find('?') != std::string_view::npos ||
         pattern.find('[') != std::string_view::npos;
}

// Matches one path component against a glob fragment supporting `*` and `?`.
bool matches_component(std::string_view pattern, std::string_view name) {
  std::size_t p = 0;
  std::size_t n = 0;
  std::size_t star_p = std::string_view::npos;
  std::size_t star_n = 0;
  while (n < name.size()) {
    if (p < pattern.size() && pattern[p] != '*' &&
        (pattern[p] == '?' || pattern[p] == name[n])) {
      ++p;
      ++n;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star_p = p++;
      star_n = n;
    } else if (star_p != std::string_view::npos) {
      p = star_p + 1;
      n = ++star_n;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

// True when `relative` is the excluded path or lives underneath it.
//
// Matching is per path component. A raw substring test was used before and
// made `exclude = ["vendor"]` also drop `vendor-lib` — and `exclude = ["a"]`
// drop every member whose path merely contained the letter "a". Both silently
// removed packages from the workspace.
bool excludes_path(const std::string& relative, const std::string& pattern) {
  const std::vector<std::string> rel_parts = str::split(relative, "/");
  const std::vector<std::string> pat_parts = str::split(pattern, "/");
  if (pat_parts.empty() || rel_parts.size() < pat_parts.size()) {
    return false;
  }
  // The excluded path must match a leading run of components.
  for (std::size_t index = 0; index < pat_parts.size(); ++index) {
    if (pat_parts[index] == "**") {
      // `**` excludes everything below the current position.
      return true;
    }
    if (!matches_component(pat_parts[index], rel_parts[index])) {
      return false;
    }
  }
  return true;
}

bool matches_any(const std::string& relative, const std::vector<std::string>& patterns) {
  for (const auto& pattern : patterns) {
    if (excludes_path(relative, pattern)) {
      return true;
    }
  }
  return false;
}

Result<manifest::Manifest> load_member_manifest(const stdfs::path& root) {
  auto loaded = manifest::load(root / "cova.toml");
  if (!loaded) {
    return fail<manifest::Manifest>(loaded.error());
  }
  return loaded->manifest;
}

}  // namespace

const MemberEntry* Workspace::find_member(std::string_view name) const {
  for (const auto& member : members) {
    if (member.name == name) {
      return &member;
    }
  }
  return nullptr;
}

manifest::ToolchainSection Workspace::toolchain_policy_for(const MemberEntry& member) const {
  auto member_manifest = load_member_manifest(member.root);
  if (member_manifest) {
    const auto& section = member_manifest->toolchain;
    if (!section.compiler.empty() || !section.version.empty() || !section.preferred.empty() ||
        section.policy != toolchain::Policy::SystemFirst) {
      return section;
    }
  }
  if (root.workspace && root.workspace->toolchain) {
    return *root.workspace->toolchain;
  }
  return root.toolchain;
}

manifest::FormatSection Workspace::format_policy_for(const MemberEntry& member) const {
  auto member_manifest = load_member_manifest(member.root);
  if (member_manifest) {
    const auto& section = member_manifest->format;
    if (section.tool != "clang-format" || section.style != "file" ||
        !section.include.empty() || !section.exclude.empty()) {
      return section;
    }
  }
  if (root.workspace && root.workspace->format) {
    return *root.workspace->format;
  }
  return root.format;
}

std::vector<std::string> Workspace::member_relpaths() const {
  std::vector<std::string> out;
  out.reserve(members.size());
  for (const auto& member : members) {
    out.push_back(fs::generic_string(member.root.lexically_relative(root_dir)));
  }
  std::sort(out.begin(), out.end());
  return out;
}

Result<std::vector<stdfs::path>> expand_members(const stdfs::path& root,
                                                const manifest::Manifest& root_manifest,
                                                const LoadOptions& options) {
  if (!root_manifest.workspace) {
    return fail<std::vector<stdfs::path>>(
        Error::make("cova::workspace::not_a_workspace",
                    "`" + fs::generic_string(root / "cova.toml") +
                        "` does not define a [workspace] table"));
  }
  const auto& section = *root_manifest.workspace;
  std::vector<stdfs::path> members;
  std::set<std::string> seen;

  auto add_member = [&](const stdfs::path& candidate) {
    if (!fs::is_directory(candidate) || !fs::is_regular_file(candidate / "cova.toml")) {
      return;
    }
    auto absolute = fs::absolute_normalized(candidate);
    if (absolute == fs::absolute_normalized(root)) {
      return;
    }
    std::error_code ec;
    auto relative = absolute.lexically_relative(fs::absolute_normalized(root));
    const std::string relative_text = fs::generic_string(relative);
    if (matches_any(relative_text, section.exclude)) {
      return;
    }
    if (seen.insert(fs::generic_string(absolute)).second) {
      members.push_back(std::move(absolute));
    }
  };

  for (const auto& pattern : section.members) {
    if (!has_glob_magic(pattern)) {
      add_member(root / pattern);
      continue;
    }
    // `fs::glob` expands files, not directories. Appending `/cova.toml` turns a
    // member directory pattern such as `packages/*` into a file glob whose
    // matches identify package roots.
    auto manifest_matches = fs::glob(root, pattern + "/cova.toml");
    if (manifest_matches) {
      for (const auto& match : *manifest_matches) {
        add_member(match.parent_path());
      }
    }
    auto matches = fs::glob(root, pattern);
    if (!matches) {
      continue;
    }
    for (const auto& match : *matches) {
      add_member(match);
      if (options.recursive) {
        for (const auto& file : fs::walk_files(match)) {
          if (file.filename() == "cova.toml") {
            add_member(file.parent_path());
          }
        }
      }
    }
  }

  std::sort(members.begin(), members.end());
  return members;
}

Result<Workspace> load(const stdfs::path& root_dir, const LoadOptions& options) {
  auto loaded = manifest::load_from_root(root_dir);
  if (!loaded) {
    return fail<Workspace>(loaded.error());
  }
  Workspace workspace;
  workspace.root = std::move(loaded->manifest);
  workspace.root_dir = fs::absolute_normalized(root_dir);
  workspace.lockfile = workspace.root_dir / "cova.lock";
  workspace.toolchain_lockfile = workspace.root_dir / "cova-toolchain.lock";

  if (!workspace.root.workspace) {
    return fail<Workspace>(
        Error::make("cova::workspace::not_a_workspace",
                    "`" + fs::generic_string(root_dir / "cova.toml") +
                        "` does not define a [workspace] table"));
  }

  auto paths = expand_members(workspace.root_dir, workspace.root, options);
  if (!paths) {
    return fail<Workspace>(paths.error());
  }
  for (const auto& path : *paths) {
    auto member_manifest = manifest::load_from_root(path);
    if (!member_manifest) {
      Error error = member_manifest.error();
      error.with("workspace member: " + fs::generic_string(path));
      return fail<Workspace>(std::move(error));
    }
    MemberEntry entry;
    entry.root = path;
    entry.name = member_manifest->manifest.package.name;
    if (entry.name.empty()) {
      entry.name = path.filename().string();
    }
    entry.kind = member_manifest->manifest.build.kind;
    entry.warnings = std::move(member_manifest->warnings);
    workspace.members.push_back(std::move(entry));
  }

  std::sort(workspace.members.begin(), workspace.members.end(),
            [](const MemberEntry& a, const MemberEntry& b) { return a.name < b.name; });
  return workspace;
}

Result<ProjectContext> discover_project(const stdfs::path& start_dir) {
  auto manifest_path = manifest::find_manifest_upwards(start_dir);
  if (!manifest_path) {
    return fail<ProjectContext>(manifest_path.error());
  }
  ProjectContext context;
  context.current_dir = fs::absolute_normalized(start_dir);
  context.manifest_path = *manifest_path;
  context.root_dir = manifest_path->parent_path();

  auto loaded = manifest::load(*manifest_path);
  if (!loaded) {
    return fail<ProjectContext>(loaded.error());
  }

  stdfs::path workspace_root;
  if (loaded->manifest.is_workspace_root()) {
    workspace_root = context.root_dir;
  } else {
    // Search above the package for a workspace that contains it.
    auto probe = context.root_dir.parent_path();
    while (!probe.empty()) {
      if (fs::is_regular_file(probe / "cova.toml")) {
        auto candidate = manifest::load(probe / "cova.toml");
        if (candidate && candidate->manifest.is_workspace_root()) {
          workspace_root = probe;
          break;
        }
      }
      const auto parent = probe.parent_path();
      if (parent == probe) {
        break;
      }
      probe = parent;
    }
  }

  if (!workspace_root.empty()) {
    auto workspace = load(workspace_root);
    if (workspace) {
      context.is_workspace = true;
      context.workspace = std::move(*workspace);
      // The member containing the starting directory, if any.
      const auto current = fs::absolute_normalized(start_dir);
      for (const auto& member : context.workspace.members) {
        if (fs::is_within(current, member.root) || current == member.root) {
          context.current_member = member;
          break;
        }
      }
    }
  }
  return context;
}

Result<std::vector<const MemberEntry*>> select_members(const Workspace& workspace,
                                                       const SelectionOptions& options) {
  std::vector<const MemberEntry*> selected;
  if (!options.packages.empty()) {
    for (const auto& name : options.packages) {
      const MemberEntry* member = workspace.find_member(name);
      if (member == nullptr) {
        // Accept a path as well as a package name.
        for (const auto& candidate : workspace.members) {
          if (fs::generic_string(candidate.root.lexically_relative(workspace.root_dir)) == name) {
            member = &candidate;
            break;
          }
        }
      }
      if (member == nullptr) {
        return fail<std::vector<const MemberEntry*>>(Error::make(
            "cova::workspace::unknown_member", "no workspace member named `" + name + "`"));
      }
      selected.push_back(member);
    }
    return selected;
  }
  if (options.workspace_wide) {
    for (const auto& member : workspace.members) {
      selected.push_back(&member);
    }
    return selected;
  }
  if (!options.current_dir.empty()) {
    const auto current = fs::absolute_normalized(options.current_dir);
    for (const auto& member : workspace.members) {
      if (fs::is_within(current, member.root)) {
        selected.push_back(&member);
        return selected;
      }
    }
  }
  for (const auto& member : workspace.members) {
    selected.push_back(&member);
  }
  return selected;
}

Result<std::vector<std::string>> member_build_order(const Workspace& workspace) {
  std::map<std::string, const MemberEntry*, std::less<>> by_name;
  for (const auto& member : workspace.members) {
    if (!by_name.emplace(member.name, &member).second) {
      return fail<std::vector<std::string>>(Error::make(
          "cova::workspace::duplicate_member", "duplicate workspace member `" + member.name + "`"));
    }
  }

  std::map<std::string, std::vector<std::string>, std::less<>> edges;
  for (const auto& member : workspace.members) {
    auto loaded = load_member_manifest(member.root);
    if (!loaded) {
      return fail<std::vector<std::string>>(loaded.error());
    }
    for (const auto& dependency : loaded->dependencies) {
      std::string target;
      if (dependency.path) {
        auto absolute = fs::absolute_normalized(*dependency.path);
        for (const auto& candidate : workspace.members) {
          if (candidate.root == absolute) {
            target = candidate.name;
            break;
          }
        }
      }
      if (target.empty() && by_name.contains(dependency.name)) {
        target = dependency.name;
      }
      if (!target.empty() && target != member.name) {
        edges[member.name].push_back(target);
      }
    }
  }

  std::vector<std::string> order;
  std::map<std::string, int, std::less<>> state;  // 0 unvisited, 1 visiting, 2 done
  std::function<Status(const std::string&)> visit = [&](const std::string& name) -> Status {
    int& current = state[name];
    if (current == 2) {
      return ok();
    }
    if (current == 1) {
      return fail(Error::make("cova::workspace::cycle",
                              "workspace dependency cycle involving `" + name + "`"));
    }
    current = 1;
    auto it = edges.find(name);
    if (it != edges.end()) {
      std::vector<std::string> dependencies = it->second;
      std::sort(dependencies.begin(), dependencies.end());
      dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
      for (const auto& dependency : dependencies) {
        auto status = visit(dependency);
        if (!status) {
          return status;
        }
      }
    }
    current = 2;
    order.push_back(name);
    return ok();
  };

  for (const auto& [name, member] : by_name) {
    (void)member;
    auto status = visit(name);
    if (!status) {
      return fail<std::vector<std::string>>(status.error());
    }
  }
  return order;
}

Result<std::vector<std::string>> validate_workspace(const Workspace& workspace) {
  std::vector<std::string> warnings;
  std::set<std::string> names;
  for (const auto& member : workspace.members) {
    if (member.name.empty()) {
      return fail<std::vector<std::string>>(
          Error::make("cova::workspace::member_name",
                      "workspace member at " + fs::generic_string(member.root) +
                          " has no package name"));
    }
    if (!names.insert(member.name).second) {
      return fail<std::vector<std::string>>(Error::make(
          "cova::workspace::duplicate_member", "duplicate workspace member `" + member.name + "`"));
    }
    if (!fs::is_regular_file(member.root / "cova.toml")) {
      return fail<std::vector<std::string>>(Error::make(
          "cova::workspace::missing_manifest",
          "workspace member `" + member.name + "` has no cova.toml"));
    }
    for (const auto& warning : member.warnings) {
      warnings.push_back(member.name + ": " + warning);
    }
  }
  auto order = member_build_order(workspace);
  if (!order) {
    return fail<std::vector<std::string>>(order.error());
  }
  if (workspace.members.empty()) {
    warnings.push_back("the workspace has no members");
  }
  return warnings;
}

}  // namespace cova::workspace
