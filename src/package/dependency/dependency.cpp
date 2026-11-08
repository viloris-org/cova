// Cova — local dependency resolution (PRD §10, §15).
#include "package/dependency/dependency.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"

namespace cova::package {
namespace {

using stdfs::path;

std::string relative_text(const path& value, const path& base) {
  std::error_code ec;
  auto relative = value.lexically_relative(base);
  if (ec || relative.empty()) {
    return fs::generic_string(value);
  }
  return fs::generic_string(relative);
}

std::string describe_source(const manifest::DependencySpec& dependency, const path& base) {
  if (dependency.path) {
    return "path:" + relative_text(*dependency.path, base);
  }
  if (dependency.git) {
    std::string text = "git:" + *dependency.git;
    if (dependency.git_rev) {
      text += "@" + *dependency.git_rev;
    }
    return text;
  }
  if (dependency.registry) {
    return "registry:" + *dependency.registry;
  }
  return "registry";
}

std::string resolution_input(const manifest::Manifest& manifest, const path& base) {
  std::string out = "name=";
  out += manifest.package.name;
  out += "\nversion=";
  out += manifest.package.version;
  out += '\n';
  for (const auto& dependency : manifest.dependencies) {
    out += "dependency=";
    out += dependency.name;
    out += '|';
    out += describe_source(dependency, base);
    out += '|';
    out += dependency.requirement_text;
    if (dependency.optional) {
      out += "|optional";
    }
    if (dependency.dev) {
      out += "|dev";
    }
    if (!dependency.target_filters.empty()) {
      std::vector<std::string> filters = dependency.target_filters;
      std::sort(filters.begin(), filters.end());
      out += "|target=";
      for (const auto& filter : filters) {
        out += filter;
        out += ',';
      }
    }
    out += '\n';
  }
  return out;
}

bool is_active_for(const manifest::Manifest& manifest, std::string_view name,
                   const platform::Triple& target) {
  for (const auto* dependency : manifest.target_dependencies(target)) {
    if (dependency->name == name) {
      return true;
    }
  }
  return false;
}

class Resolver {
 public:
  explicit Resolver(const ResolveRequest& request) : request_(request) {}

  Result<DependencyGraph> run() {
    auto members = register_workspace_members();
    if (!members) {
      return fail<DependencyGraph>(members.error());
    }

    std::vector<const manifest::Manifest*> roots;
    roots.reserve(request_.roots.size());
    for (const auto& root : request_.roots) {
      if (root.package.name.empty()) {
        return fail<DependencyGraph>(
            Error::make("cova::package::missing_name",
                        "a package at " + fs::generic_string(root.root) +
                            " has no `package.name` and cannot be a build root"));
      }
      roots.push_back(&root);
      root_names_.insert(root.package.name);
    }
    std::sort(roots.begin(), roots.end(),
              [](const manifest::Manifest* a, const manifest::Manifest* b) {
                return a->package.name < b->package.name;
              });

    for (const auto* root : roots) {
      if (!manifests_.contains(root->package.name)) {
        manifests_.emplace(root->package.name, *root);
        members_by_root_.emplace(fs::generic_string(fs::absolute_normalized(root->root)),
                                 root->package.name);
      }
      auto status = visit(root->package.name);
      if (!status) {
        return fail<DependencyGraph>(status.error());
      }
    }

    DependencyGraph graph;
    for (const auto& name : order_) {
      ResolvedPackage package;
      package.manifest = manifests_.at(name);
      package.all_dependencies = all_dependencies_[name];
      package.build_dependencies = build_dependencies_[name];
      package.root = root_names_.contains(name);
      package.resolution_input = resolution_input(package.manifest, request_.lock_root);
      graph.packages.push_back(std::move(package));
    }
    graph.fingerprint = fingerprint(graph.packages);
    return graph;
  }

 private:
  Status register_workspace_members() {
    for (const auto& member : request_.workspace_members) {
      if (member.package.name.empty()) {
        return fail(Error::make(
            "cova::package::missing_name",
            "a workspace member at " + fs::generic_string(member.root) +
                " has no `package.name` and cannot be a dependency"));
      }
      auto [it, inserted] = manifests_.emplace(member.package.name, member);
      if (!inserted) {
        return fail(Error::make("cova::package::duplicate",
                                "two workspace members are named `" + member.package.name + "`")
                       .with("first: " + fs::generic_string(it->second.root))
                       .with("second: " + fs::generic_string(member.root)));
      }
      members_by_name_[member.package.name] = member.package.name;
      members_by_root_.emplace(fs::generic_string(fs::absolute_normalized(member.root)),
                               member.package.name);
    }
    return ok();
  }

  // Loads the package a dependency declaration points at. Only local path
  // dependencies and name-only references to workspace members are supported.
  Result<manifest::Manifest> load_dependency_package(const manifest::DependencySpec& dependency) {
    if (dependency.path) {
      const path absolute = fs::absolute_normalized(*dependency.path);
      auto member = members_by_root_.find(fs::generic_string(absolute));
      if (member != members_by_root_.end()) {
        return manifests_.at(member->second);
      }
      const path manifest_path = absolute / "cova.toml";
      if (!fs::is_regular_file(manifest_path)) {
        return fail<manifest::Manifest>(
            Error::make("cova::package::dependency_missing",
                        "dependency `" + dependency.name + "` has no manifest")
                .with("expected: " + fs::generic_string(manifest_path))
                .with("a path dependency must point at a directory containing cova.toml"));
      }
      auto loaded = manifest::load(manifest_path);
      if (!loaded) {
        Error error = loaded.error();
        error.with("dependency: " + dependency.name + " (path " + fs::generic_string(absolute) + ")");
        return fail<manifest::Manifest>(std::move(error));
      }
      return loaded->manifest;
    }
    auto member = members_by_name_.find(dependency.name);
    if (member != members_by_name_.end()) {
      return manifests_.at(member->second);
    }
    Error error = Error::make(
        "cova::package::not_implemented",
        "dependency `" + dependency.name + "` is not a local path dependency");
    error.with("remote dependency management (registry and git sources) is not implemented yet");
    error.with("declare it as `" + dependency.name +
               " = { path = \"../path/to/package\" }`, or remove it from cova.toml");
    return fail<manifest::Manifest>(std::move(error));
  }

  Status visit(const std::string& name) {
    int& state = state_[name];
    if (state == 2) {
      return ok();
    }
    if (state == 1) {
      return fail(cycle_error(name));
    }
    state = 1;
    stack_.push_back(name);

    const manifest::Manifest& manifest = manifests_.at(name);
    std::vector<std::string> build_dependencies;
    std::vector<std::string> all_dependencies;
    for (const auto& dependency : manifest.dependencies) {
      const bool local = dependency.path.has_value() ||
                         members_by_name_.contains(dependency.name);
      if (dependency.dev && !request_.include_dev && !local) {
        // A remote development dependency is irrelevant to a regular build and
        // cannot be locked without a fetcher; skip it instead of failing a
        // build that does not need it.
        continue;
      }
      auto child = load_dependency_package(dependency);
      if (!child) {
        return fail(child.error());
      }
      if (child->package.name.empty()) {
        return fail(Error::make("cova::package::missing_name",
                                "a dependency of `" + name + "` has no `package.name`"));
      }
      if (child->package.name != dependency.name) {
        return fail(Error::make("cova::package::name_mismatch",
                                "dependency `" + dependency.name + "` points at a package named `" +
                                    child->package.name + "`")
                       .with("manifest: " + fs::generic_string(child->path)));
      }
      const path child_root = fs::absolute_normalized(child->root);
      auto existing = manifests_.find(child->package.name);
      if (existing == manifests_.end()) {
        manifests_.emplace(child->package.name, *child);
        members_by_root_.emplace(fs::generic_string(child_root), child->package.name);
      } else if (fs::generic_string(fs::absolute_normalized(existing->second.root)) !=
                 fs::generic_string(child_root)) {
        return fail(Error::make("cova::package::duplicate",
                                "package `" + child->package.name +
                                    "` is declared from two different paths")
                       .with("first: " + fs::generic_string(existing->second.root))
                       .with("second: " + fs::generic_string(child_root))
                       .with("Cova cannot build two packages with the same name"));
      }

      all_dependencies.push_back(child->package.name);
      const bool build_edge = !dependency.dev || request_.include_dev;
      if (build_edge && is_active_for(manifest, dependency.name, request_.target)) {
        build_dependencies.push_back(child->package.name);
      }
      auto status = visit(child->package.name);
      if (!status) {
        return status;
      }
    }

    std::sort(all_dependencies.begin(), all_dependencies.end());
    std::sort(build_dependencies.begin(), build_dependencies.end());
    all_dependencies_[name] = std::move(all_dependencies);
    build_dependencies_[name] = std::move(build_dependencies);
    stack_.pop_back();
    state = 2;
    order_.push_back(name);
    return ok();
  }

  Error cycle_error(const std::string& repeated) const {
    std::string chain;
    auto start = std::find(stack_.begin(), stack_.end(), repeated);
    for (auto it = start; it != stack_.end(); ++it) {
      chain += *it;
      chain += " -> ";
    }
    chain += repeated;
    return Error::make("cova::package::cycle",
                       "local dependency cycle detected: " + chain)
        .with("path dependencies must form a directed acyclic graph");
  }

  static std::string fingerprint(const std::vector<ResolvedPackage>& packages) {
    std::vector<std::pair<std::string, std::string>> inputs;
    inputs.reserve(packages.size());
    for (const auto& package : packages) {
      inputs.emplace_back(package.manifest.package.name, package.resolution_input);
    }
    std::sort(inputs.begin(), inputs.end());
    Fingerprint builder;
    for (const auto& [name, input] : inputs) {
      builder.add(name);
      builder.add(input);
    }
    return builder.finish_hex();
  }

  const ResolveRequest& request_;
  std::map<std::string, manifest::Manifest, std::less<>> manifests_;
  std::map<std::string, std::string, std::less<>> members_by_root_;
  std::map<std::string, std::string, std::less<>> members_by_name_;
  std::map<std::string, int, std::less<>> state_;
  std::map<std::string, std::vector<std::string>, std::less<>> all_dependencies_;
  std::map<std::string, std::vector<std::string>, std::less<>> build_dependencies_;
  std::set<std::string, std::less<>> root_names_;
  std::vector<std::string> stack_;
  std::vector<std::string> order_;
};

std::vector<std::string> compare_locks(const lockfile::DependencyLock& expected,
                                       const lockfile::DependencyLock& actual) {
  std::vector<std::string> differences;
  if (expected.version != actual.version) {
    differences.push_back("lockfile format changed: " + actual.version + " -> " + expected.version);
  }
  if (expected.manifest_fingerprint != actual.manifest_fingerprint) {
    differences.push_back("recorded dependency inputs changed");
  }

  std::map<std::string, const lockfile::LockedPackage*, std::less<>> actual_by_name;
  for (const auto& package : actual.packages) {
    actual_by_name[package.name] = &package;
  }
  for (const auto& package : expected.packages) {
    auto it = actual_by_name.find(package.name);
    if (it == actual_by_name.end()) {
      differences.push_back("`" + package.name + "` is not recorded");
      continue;
    }
    actual_by_name.erase(it);
    const lockfile::LockedPackage& recorded = *it->second;
    auto differ = [&](const std::string& what) {
      differences.push_back("`" + package.name + "` " + what);
    };
    if (recorded.version != package.version) {
      differ("version changed: " + recorded.version + " -> " + package.version);
    }
    if (recorded.path != package.path) {
      differ("path changed: " + recorded.path.value_or("<none>") + " -> " +
             package.path.value_or("<none>"));
    }
    if (recorded.source_url != package.source_url || recorded.git_rev != package.git_rev ||
        recorded.registry != package.registry) {
      differ("source changed");
    }
    if (recorded.dependencies != package.dependencies) {
      differ("dependency list changed");
    }
    if (recorded.source_hash != package.source_hash) {
      differ("manifest content changed");
    }
    if (recorded.dev != package.dev) {
      differ("development flag changed");
    }
  }
  for (const auto& [name, recorded] : actual_by_name) {
    (void)recorded;
    differences.push_back("`" + name + "` is recorded but no longer resolved");
  }
  return differences;
}

}  // namespace

const ResolvedPackage* DependencyGraph::find(std::string_view name) const {
  for (const auto& package : packages) {
    if (package.manifest.package.name == name) {
      return &package;
    }
  }
  return nullptr;
}

Result<DependencyGraph> resolve(const ResolveRequest& request) {
  Resolver resolver(request);
  return resolver.run();
}

lockfile::DependencyLock to_lock(const DependencyGraph& graph, const stdfs::path& lock_root) {
  lockfile::DependencyLock lock;
  lock.version = "1";
  lock.generated_by = "cova";
  lock.manifest_fingerprint = graph.fingerprint;

  std::vector<const ResolvedPackage*> sorted;
  sorted.reserve(graph.packages.size());
  for (const auto& package : graph.packages) {
    sorted.push_back(&package);
  }
  std::sort(sorted.begin(), sorted.end(), [](const ResolvedPackage* a, const ResolvedPackage* b) {
    return a->manifest.package.name < b->manifest.package.name;
  });

  for (const auto* package : sorted) {
    lockfile::LockedPackage entry;
    entry.name = package->manifest.package.name;
    entry.version = package->manifest.package.version;
    entry.path = relative_text(fs::absolute_normalized(package->manifest.root),
                               fs::absolute_normalized(lock_root));
    entry.dependencies = package->all_dependencies;
    // A path dependency is live source, not fetched content: there is no
    // verified content hash to record. Only the resolved identity is locked.
    lock.packages.push_back(std::move(entry));
  }
  return lock;
}

Result<LockOutcome> enforce_lock(const DependencyGraph& graph, const stdfs::path& lock_path,
                                 bool locked_mode) {
  LockOutcome outcome;
  const auto lock_root = lock_path.has_parent_path() ? lock_path.parent_path() : stdfs::path(".");
  lockfile::DependencyLock expected = to_lock(graph, lock_root);
  outcome.present = fs::is_regular_file(lock_path);

  if (!outcome.present) {
    if (locked_mode) {
      return fail<LockOutcome>(
          Error::make("cova::lock::missing",
                      "no `cova.lock` found but locked mode was requested")
              .with("path: " + fs::generic_string(lock_path))
              .with("run without --locked to resolve dependencies and write a lockfile"));
    }
    auto status = expected.save(lock_path);
    if (!status) {
      return fail<LockOutcome>(status.error());
    }
    outcome.lock = std::move(expected);
    outcome.wrote = true;
    outcome.up_to_date = true;
    return outcome;
  }

  auto loaded = lockfile::DependencyLock::load(lock_path);
  if (!loaded) {
    if (locked_mode) {
      Error error = loaded.error();
      error.with("run without --locked to rewrite it");
      return fail<LockOutcome>(std::move(error));
    }
    auto status = expected.save(lock_path);
    if (!status) {
      return fail<LockOutcome>(status.error());
    }
    outcome.lock = std::move(expected);
    outcome.wrote = true;
    outcome.up_to_date = true;
    return outcome;
  }

  outcome.lock = *loaded;
  outcome.differences = compare_locks(expected, *loaded);
  if (outcome.differences.empty()) {
    outcome.up_to_date = true;
    return outcome;
  }
  if (locked_mode) {
    Error error = Error::make(
        "cova::lock::stale",
        "`cova.lock` is out of date and locked mode forbids rewriting it");
    error.with("path: " + fs::generic_string(lock_path));
    for (const auto& difference : outcome.differences) {
      error.with(difference);
    }
    error.with("run without --locked to resolve dependencies and update the lockfile");
    return fail<LockOutcome>(std::move(error));
  }
  auto status = expected.save(lock_path);
  if (!status) {
    return fail<LockOutcome>(status.error());
  }
  outcome.lock = std::move(expected);
  outcome.wrote = true;
  return outcome;
}

}  // namespace cova::package
