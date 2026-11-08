// Cova — Milestone 3 command surface (PRD §10, §14).
//
// These commands exist so the CLI contract is stable early, but this milestone
// has no registry infrastructure and no dependency fetcher. Every command that
// would need one reports precisely that instead of pretending to work. Commands
// that are purely local (`tree` for path dependencies, `vendor`) do real work.
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "build/cache/cache.hpp"
#include "cli/common.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/json/json.hpp"
#include "manifest/manifest.hpp"

namespace cova::cli {
namespace {

int report_milestone_gap(CommandContext& ctx, std::string_view command,
                        std::string_view what) {
  DiagnosticEngine engine;
  Diagnostic diagnostic = make_error(
      "cova::package::not_implemented",
      "`cova " + std::string(command) + "` is not available in this milestone");
  diagnostic.note("missing capability", std::string(what));
  diagnostic.line(
      "Cova's package manager needs a trusted registry and a verified fetcher; neither "
      "exists yet. Rather than simulate them, this command fails explicitly (PRD §18.1, "
      "OD-003).");
  diagnostic.line("local `path` dependencies in [dependencies] are declared but not yet "
                  "consumed by the build graph.");
  engine.emit(std::move(diagnostic));
  return report_diagnostics(engine, ctx);
}

// Renders `cova tree` for declared dependencies without resolving them.
struct TreeNode {
  std::string label;
  std::vector<TreeNode> children;
};

void render_tree(const TreeNode& node, std::string prefix, bool last, std::string& out,
                 bool is_root) {
  if (is_root) {
    out += node.label;
    out += '\n';
  } else {
    out += prefix;
    out += last ? "└── " : "├── ";
    out += node.label;
    out += '\n';
  }
  const std::string child_prefix = is_root ? prefix : prefix + (last ? "    " : "│   ");
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    render_tree(node.children[i], child_prefix, i + 1 == node.children.size(), out, false);
  }
}

}  // namespace

int cmd_add(CommandContext& ctx) {
  if (ctx.positionals().empty()) {
    ctx.ui->error("`cova add` requires a package name");
    ctx.ui->info("usage: cova add <name>[@<version>] [--path <dir> | --git <url> [--rev <rev>]]");
    return 2;
  }
  // Declaring a path dependency is a purely local manifest edit and is allowed.
  const bool local = ctx.flags.has("path");
  if (!local) {
    return report_milestone_gap(ctx, "add",
                                "dependency resolution and source fetching from a registry");
  }

  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  std::string name = ctx.positionals().front();
  std::string version;
  if (auto at = name.find('@'); at != std::string::npos) {
    version = name.substr(at + 1);
    name = name.substr(0, at);
  }

  manifest::Manifest manifest = project->manifest;
  for (auto& dependency : manifest.dependencies) {
    if (dependency.name == name) {
      return report_error(Error::make("cova::add::duplicate",
                                      "`" + name + "` is already a dependency"),
                          ctx);
    }
  }
  manifest::DependencySpec spec;
  spec.name = name;
  spec.path = fs::absolute_normalized(ctx.cwd / *ctx.flags.get("path"));
  spec.requirement_text = version;
  spec.dev = ctx.flags.boolean("dev");
  manifest.dependencies.push_back(std::move(spec));

  auto status = fs::write_file_atomic(project->layout.manifest, manifest::to_toml(manifest));
  if (!status) {
    return report_error(status.error(), ctx);
  }
  ctx.ui->info("added path dependency `" + name + "`");
  ctx.ui->verbose("note: the build graph does not consume dependencies yet in this milestone");
  return 0;
}

int cmd_remove(CommandContext& ctx) {
  if (ctx.positionals().empty()) {
    ctx.ui->error("`cova remove` requires a package name");
    return 2;
  }
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  const std::string name = ctx.positionals().front();
  manifest::Manifest manifest = project->manifest;
  const auto before = manifest.dependencies.size();
  manifest.dependencies.erase(
      std::remove_if(manifest.dependencies.begin(), manifest.dependencies.end(),
                     [&](const manifest::DependencySpec& spec) { return spec.name == name; }),
      manifest.dependencies.end());
  if (manifest.dependencies.size() == before) {
    return report_error(
        Error::make("cova::remove::unknown", "`" + name + "` is not a declared dependency"), ctx);
  }
  auto status = fs::write_file_atomic(project->layout.manifest, manifest::to_toml(manifest));
  if (!status) {
    return report_error(status.error(), ctx);
  }
  ctx.ui->info("removed dependency `" + name + "`");
  return 0;
}

int cmd_fetch(CommandContext& ctx) {
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  bool has_remote = false;
  for (const auto& dependency : project->manifest.dependencies) {
    if (!dependency.is_local()) {
      has_remote = true;
      break;
    }
  }
  if (!has_remote) {
    ctx.ui->info("no remote dependencies to fetch");
    return 0;
  }
  return report_milestone_gap(ctx, "fetch", "a verified source fetcher and registry protocol");
}

int cmd_update(CommandContext& ctx) {
  return report_milestone_gap(ctx, "update",
                              "a dependency resolver and a registry to resolve against");
}

int cmd_vendor(CommandContext& ctx) {
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  std::vector<const manifest::DependencySpec*> local_deps;
  for (const auto& dependency : project->manifest.dependencies) {
    if (dependency.is_local()) {
      local_deps.push_back(&dependency);
    }
  }
  if (local_deps.empty()) {
    auto status = fs::create_directories(project->layout.vendor_dir);
    if (!status) {
      return report_error(status.error(), ctx);
    }
    auto ignore = fs::write_file(project->layout.vendor_dir / ".gitignore",
                                 "# Cova vendored sources\n");
    (void)ignore;
    ctx.ui->info("created " + display_path(project->layout.vendor_dir, project->layout.root) +
                 " with no dependencies to vendor");
    return 0;
  }
  return report_milestone_gap(ctx, "vendor",
                              "source fetching; only remote dependencies can be vendored");
}

int cmd_tree(CommandContext& ctx) {
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }

  const auto& manifest = project->manifest;
  if (ctx.global.json) {
    json::Value root = json::Value::object();
    root.set("name", manifest.package.name);
    root.set("version", manifest.package.version);
    auto deps = json::Value::array();
    for (const auto& dependency : manifest.dependencies) {
      json::Value entry = json::Value::object();
      entry.set("name", dependency.name);
      entry.set("requirement", dependency.requirement_text);
      entry.set("kind", dependency.is_local() ? "path"
                                              : (dependency.is_git() ? "git" : "registry"));
      entry.set("dev", dependency.dev);
      deps.push(std::move(entry));
    }
    root.set("dependencies", std::move(deps));
    root.set("resolved", false);
    ctx.ui->out(root.dump(2));
    return 0;
  }

  TreeNode root;
  root.label = manifest.package.name + " v" + manifest.package.version;
  for (const auto& dependency : manifest.dependencies) {
    TreeNode child;
    std::string source;
    if (dependency.is_local()) {
      source = " (path " + fs::generic_string(*dependency.path) + ")";
    } else if (dependency.is_git()) {
      source = " (git " + *dependency.git + ")";
    } else {
      source = " (" + dependency.requirement_text + ")";
    }
    if (dependency.dev) {
      source += " [dev]";
    }
    child.label = dependency.name + source;
    root.children.push_back(std::move(child));
  }

  std::string rendered;
  render_tree(root, "", true, rendered, true);
  ctx.ui->out(rendered);

  if (!manifest.dependencies.empty()) {
    ctx.ui->warn(
        "dependencies are declared but not yet resolved or built in this milestone; "
        "transitive dependencies are unknown");
  }
  return 0;
}

int cmd_install(CommandContext& ctx) {
  return report_milestone_gap(ctx, "install",
                              "a Cova-managed install prefix and manifest-defined install rules");
}

int cmd_package(CommandContext& ctx) {
  return report_milestone_gap(ctx, "package", "an artifact packaging pipeline (PRD §4.1, P2)");
}

int cmd_publish(CommandContext& ctx) {
  return report_milestone_gap(ctx, "publish", "a registry with authentication (PRD §4.1, P3)");
}

int cmd_cache(CommandContext& ctx) {
  auto dirs = fs::cova_dirs();
  if (!dirs) {
    return report_error(dirs.error(), ctx);
  }
  build::CacheSettings settings = build::default_cache_settings();

  std::uint64_t total_bytes = 0;
  std::size_t entries = 0;
  std::error_code ec;
  if (fs::is_directory(settings.root)) {
    for (auto it = stdfs::recursive_directory_iterator(settings.root, ec);
         it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec)) {
        total_bytes += it->file_size(ec);
        ++entries;
      }
    }
  }

  if (ctx.flags.boolean("dry-run")) {
    ctx.ui->out("build cache: " + fs::generic_string(settings.root));
    ctx.ui->out("  " + std::to_string(entries) + " file(s), " + human_bytes(total_bytes));
    ctx.ui->info("dry run: nothing was removed");
    return 0;
  }

  if (!fs::is_within(settings.root, dirs->cache)) {
    return report_error(Error::make("cova::cache::unsafe_root",
                                    "refusing to clean a cache outside the Cova data directory"),
                        ctx);
  }
  if (!fs::exists(settings.root)) {
    ctx.ui->info("build cache is already empty");
    return 0;
  }
  auto removed = fs::remove_tree(settings.root, dirs->cache);
  if (!removed) {
    return report_error(removed.error(), ctx);
  }
  ctx.ui->info("reclaimed " + human_bytes(total_bytes) + " from the build cache");
  return 0;
}

}  // namespace cova::cli

namespace cova::cli {

int cmd_workspace(CommandContext& ctx) {
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  if (!project->workspace) {
    ctx.ui->info("this directory is not part of a workspace");
    ctx.ui->out("add a [workspace] table with a `members` list to make it one (PRD §14.1)");
    return 0;
  }
  const auto& workspace = *project->workspace;

  auto order = workspace::member_build_order(workspace);
  if (!order) {
    return report_error(order.error(), ctx);
  }

  if (ctx.global.json) {
    json::Value root = json::Value::object();
    root.set("root", fs::generic_string(workspace.root_dir));
    auto members = json::Value::array();
    for (const auto& name : *order) {
      auto* member = workspace.find_member(name);
      json::Value entry = json::Value::object();
      entry.set("name", name);
      if (member != nullptr) {
        entry.set("path", fs::generic_string(member->root));
        entry.set("kind", std::string(manifest::to_string(member->kind)));
      }
      members.push(std::move(entry));
    }
    root.set("members", std::move(members));
    root.set("build_order", [&] {
      json::Value array = json::Value::array();
      for (const auto& name : *order) {
        array.push(name);
      }
      return array;
    }());
    ctx.ui->out(root.dump(2));
    return 0;
  }

  ctx.ui->out("workspace: " + fs::generic_string(workspace.root_dir));
  ctx.ui->out("members (" + std::to_string(order->size()) + "), in build order:");
  for (const auto& name : *order) {
    auto* member = workspace.find_member(name);
    std::string line = "  " + name;
    if (member != nullptr) {
      line += "  " + display_path(member->root, workspace.root_dir);
      line += "  " + std::string(manifest::to_string(member->kind));
    }
    ctx.ui->out(line);
  }
  return 0;
}

}  // namespace cova::cli
