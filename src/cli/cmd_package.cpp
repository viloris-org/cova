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

namespace {

// --- surgical cova.toml editing -------------------------------------------
//
// `cova add` / `cova remove` used to rewrite the manifest from the parsed model.
// That dropped comments, reordered every table and deleted unknown keys, so a
// one-line dependency edit could destroy hand-written configuration. These
// helpers edit only the `[dependencies]` table and leave the rest byte for byte.

struct TableSpan {
  bool found = false;
  std::size_t header_line = 0;  // index of the "[dependencies]" line
  std::size_t body_begin = 0;   // first line after the header
  std::size_t body_end = 0;     // one past the last line of the body
};

// Finds the line range of the `[dependencies]` table. Only the plain table form
// is handled; dotted keys and `[dependencies.x]` sub-tables are left alone.
TableSpan find_dependencies_table(const std::vector<std::string>& lines) {
  TableSpan span;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string trimmed = str::trim(lines[i]);
    if (trimmed != "[dependencies]") {
      continue;
    }
    span.found = true;
    span.header_line = i;
    std::size_t j = i + 1;
    for (; j < lines.size(); ++j) {
      const std::string next = str::trim(lines[j]);
      if (!next.empty() && next.front() == '[') {
        break;
      }
    }
    span.body_begin = i + 1;
    span.body_end = j;
    return span;
  }
  return span;
}

// True when the line declares `name` as a top-level dependency key.
bool line_declares(const std::string& line, std::string_view name) {
  const std::string trimmed = str::trim(line);
  if (trimmed.empty() || trimmed.front() == '#') {
    return false;
  }
  const std::size_t equals = trimmed.find('=');
  if (equals == std::string::npos) {
    return false;
  }
  std::string key = str::trim(trimmed.substr(0, equals));
  if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'')) {
    key = key.substr(1, key.size() - 2);
  }
  return key == name;
}

// Escapes a value for a basic TOML string literal.
std::string escape_toml_string(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

// A path dependency is stored relative to the manifest, which keeps the file
// portable and avoids leaking an absolute home directory into a repository.
std::string relative_dependency_path(const stdfs::path& dependency,
                                    const stdfs::path& manifest_root) {
  std::error_code ec;
  const auto relative = stdfs::relative(dependency, manifest_root, ec);
  if (ec || relative.empty()) {
    return fs::generic_string(dependency);
  }
  return fs::generic_string(relative);
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t newline = text.find('\n', begin);
    if (newline == std::string::npos) {
      if (begin < text.size()) {
        lines.push_back(text.substr(begin));
      }
      break;
    }
    lines.push_back(text.substr(begin, newline - begin));
    begin = newline + 1;
  }
  return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) {
    out += line;
    out += '\n';
  }
  return out;
}

// Appends a dependency declaration, creating the table when it is missing.
Status append_dependency_line(std::vector<std::string>& lines, const std::string& name,
                              const std::string& value) {
  const std::string declaration = name + " = " + value;
  TableSpan span = find_dependencies_table(lines);
  if (!span.found) {
    if (!lines.empty() && !lines.back().empty()) {
      lines.emplace_back();
    }
    lines.push_back("[dependencies]");
    lines.push_back(declaration);
    return ok();
  }
  // Insert after the last non-blank line of the body so trailing comments on the
  // table stay at the end.
  std::size_t insert_at = span.body_begin;
  for (std::size_t i = span.body_begin; i < span.body_end; ++i) {
    if (!str::trim(lines[i]).empty()) {
      insert_at = i + 1;
    }
  }
  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), declaration);
  return ok();
}

// True when the `[dependencies]` table no longer declares anything.
bool table_is_empty(const std::vector<std::string>& lines, const TableSpan& span) {
  for (std::size_t i = span.body_begin; i < span.body_end; ++i) {
    const std::string trimmed = str::trim(lines[i]);
    if (!trimmed.empty() && trimmed.front() != '#') {
      return false;
    }
  }
  return true;
}

// Removes the declaration of `name`; leaves everything else untouched.
//
// When that empties the table, the header and its now-blank body go too, so the
// file does not accumulate an empty `[dependencies]` section on every remove.
bool remove_dependency_line(std::vector<std::string>& lines, std::string_view name) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!line_declares(lines[i], name)) {
      continue;
    }
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
    const TableSpan span = find_dependencies_table(lines);
    if (span.found && table_is_empty(lines, span)) {
      lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(span.body_begin),
                  lines.begin() + static_cast<std::ptrdiff_t>(span.body_end));
      lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(span.header_line));
      // Drop the blank separator that preceded the removed table.
      if (span.header_line > 0 && str::trim(lines[span.header_line - 1]).empty()) {
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(span.header_line - 1));
      }
    }
    return true;
  }
  return false;
}

}  // namespace

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
  diagnostic.line("remote dependency management is not implemented yet; only local path "
                  "dependencies are resolved and built.");
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
  // Capture the path before `spec` is moved into the model.
  const stdfs::path dependency_path = *spec.path;
  manifest.dependencies.push_back(std::move(spec));

  // Edit the existing text instead of re-serialising the model, so comments,
  // key order and unknown keys survive.
  auto existing = fs::read_file(project->layout.manifest);
  if (!existing) {
    return report_error(existing.error(), ctx);
  }
  std::vector<std::string> lines = split_lines(*existing);
  std::string value = "{ path = \"" +
                      escape_toml_string(relative_dependency_path(dependency_path,
                                                                  project->layout.root)) +
                      "\"";
  if (!version.empty()) {
    value += ", version = \"" + escape_toml_string(version) + "\"";
  }
  if (spec.dev) {
    value += ", dev = true";
  }
  value += " }";
  if (Status appended = append_dependency_line(lines, name, value); !appended) {
    return report_error(appended.error(), ctx);
  }
  auto status = fs::write_file_atomic(project->layout.manifest, join_lines(lines));
  if (!status) {
    return report_error(status.error(), ctx);
  }
  ctx.ui->info("added path dependency `" + name + "`");
  ctx.ui->verbose("note: `cova build` compiles and links local path dependencies; remote "
                  "dependency management is not implemented yet");
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
  auto existing = fs::read_file(project->layout.manifest);
  if (!existing) {
    return report_error(existing.error(), ctx);
  }
  std::vector<std::string> lines = split_lines(*existing);
  if (!remove_dependency_line(lines, name)) {
    return report_error(
        Error::make("cova::remove::unknown", "`" + name + "` is not a declared dependency"), ctx);
  }
  auto status = fs::write_file_atomic(project->layout.manifest, join_lines(lines));
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
  auto target = resolve_target_from(ctx, *project);
  if (!target) {
    return report_error(target.error(), ctx);
  }
  // Fetching a path dependency means loading its manifest and recording the
  // resolution; no network transport exists, so remote declarations are
  // rejected by the resolver.
  auto dependencies = resolve_dependencies(ctx, *project, *target, /*apply_lock=*/true,
                                           /*include_dev=*/true);
  if (!dependencies) {
    return report_error(dependencies.error(), ctx);
  }
  if (dependencies->lock_written) {
    ctx.ui->info("wrote " + display_path(dependencies->lockfile, project->layout.root));
  }
  std::size_t remote_free = 0;
  for (const auto& package : dependencies->graph.packages) {
    if (!package.root) {
      ++remote_free;
    }
  }
  if (remote_free == 0) {
    ctx.ui->info("no dependencies to fetch");
  } else {
    ctx.ui->info("resolved " + std::to_string(remote_free) +
                 " local path dependency package(s); nothing to download");
  }
  return 0;
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
        "`cova tree` lists declared dependencies only; local path dependencies are resolved "
        "and built by `cova build`, remote dependency management is not implemented yet");
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
