// Cova — `cova fmt`, `cova clean`, `cova graph`, `cova explain`, `cova metadata`
// (PRD §11, §12, §4.1, §19.3).
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "build/planner.hpp"
#include "build/scheduler/scheduler.hpp"
#include "cli/common.hpp"
#include "core/diagnostics/diagnostic.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/json/json.hpp"
#include "formatter/formatter.hpp"

namespace cova::cli {
namespace {

// Renders a table-ish two-column row, aligned by the widest left cell.
struct Rows {
  std::vector<std::pair<std::string, std::string>> entries;
  std::size_t width = 0;

  void add(std::string left, std::string right) {
    width = std::max(width, left.size());
    entries.emplace_back(std::move(left), std::move(right));
  }
  std::string render(std::string_view indent = "  ") const {
    std::string out;
    for (const auto& [left, right] : entries) {
      out.append(indent);
      out.append(left);
      out.append(width - left.size(), ' ');
      out.append("  ");
      out.append(right);
      out.push_back('\n');
    }
    return out;
  }
};

}  // namespace

int cmd_fmt(CommandContext& ctx) {
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  for (const auto& warning : project->warnings) {
    ctx.ui->warn(warning);
  }

  formatter::ResolveOptions resolve_options;
  resolve_options.tool = project->manifest.format.tool;
  formatter::ResolveOptions& ro = resolve_options;
  if (auto explicit_tool = ctx.flags.get("tool"); explicit_tool && !explicit_tool->empty()) {
    ro.tool = *explicit_tool;
  }
  ro.allow_install = !ctx.global.offline_mode() && !ctx.global.no_install;
  ro.offline = ctx.global.offline_mode();
  if (auto dirs = fs::cova_dirs()) {
    ro.managed_root = dirs->toolchains;
  }

  auto descriptor = formatter::resolve_formatter(ro);
  if (!descriptor) {
    return report_error(descriptor.error(), ctx);
  }
  ctx.ui->verbose("formatter: " + fs::generic_string(descriptor->path) + " (" +
                  descriptor->source + ")");

  formatter::FmtRequest request;
  request.package_root = project->layout.root;
  request.style = project->manifest.format.style;
  request.fallback_style = project->manifest.format.fallback_style;
  request.jobs = ctx.global.jobs;

  if (ctx.flags.boolean("check")) {
    request.mode = formatter::FmtMode::Check;
  } else if (ctx.flags.boolean("diff")) {
    request.mode = formatter::FmtMode::Diff;
  } else {
    request.mode = formatter::FmtMode::Write;
  }
  if (auto explicit_style = ctx.flags.get("style"); explicit_style && !explicit_style->empty()) {
    request.style = *explicit_style;
  }

  // Collect files: either the explicit positionals or the package source set.
  if (!ctx.positionals().empty()) {
    for (const auto& positional : ctx.positionals()) {
      const auto candidate = fs::absolute_normalized(ctx.cwd / positional);
      if (fs::is_directory(candidate)) {
        for (const auto& file : fs::walk_files(candidate)) {
          if (formatter::is_formattable(file)) {
            request.files.push_back(file);
          }
        }
      } else if (formatter::is_formattable(candidate)) {
        request.files.push_back(candidate);
      }
    }
  } else {
    formatter::FileSetOptions file_options;
    file_options.include_globs = project->manifest.format.include;
    file_options.exclude_globs = project->manifest.format.exclude;
    file_options.excluded_roots.push_back(project->layout.target_dir);
    if (fs::is_directory(project->layout.vendor_dir)) {
      file_options.excluded_roots.push_back(project->layout.vendor_dir);
    }
    auto files = formatter::collect_files(project->layout.root, project->manifest, file_options);
    if (!files) {
      return report_error(files.error(), ctx);
    }
    request.files = std::move(*files);
  }

  if (request.files.empty()) {
    ctx.ui->info("no source files to format");
    return 0;
  }

  auto result = formatter::format_files(*descriptor, request);
  if (!result) {
    return report_error(result.error(), ctx);
  }

  if (ctx.global.json) {
    ctx.ui->out(result->to_json());
    return result->success ? 0 : 1;
  }

  if (request.mode == formatter::FmtMode::Diff) {
    for (const auto& file : result->files) {
      if (!file.diff_text.empty()) {
        ctx.ui->out(file.diff_text);
      }
    }
  } else if (request.mode == formatter::FmtMode::Check) {
    if (result->changed == 0 && result->failed == 0) {
      ctx.ui->info("all " + std::to_string(result->files.size()) + " file(s) are formatted");
    } else {
      for (const auto& file : result->files) {
        if (file.changed) {
          ctx.ui->out("  would reformat " + display_path(file.file, project->layout.root));
        }
        if (file.failed) {
          ctx.ui->error("could not format " +
                        display_path(file.file, project->layout.root) + ": " + file.error);
        }
      }
      ctx.ui->error("formatting required for " + std::to_string(result->changed) + " file(s)");
    }
  } else {
    for (const auto& file : result->files) {
      if (file.changed) {
        ctx.ui->out("  formatted " + display_path(file.file, project->layout.root));
      }
      if (file.failed) {
        ctx.ui->error("could not format " + display_path(file.file, project->layout.root) + ": " +
                      file.error);
      }
    }
    ctx.ui->info("formatted " + std::to_string(result->changed) + " of " +
                 std::to_string(result->files.size()) + " file(s)");
  }

  return result->success ? 0 : 1;
}

int cmd_clean(CommandContext& ctx) {
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }

  const bool dry_run = ctx.flags.boolean("dry-run");
  const bool clean_all = ctx.flags.boolean("all");
  const auto target_dir = project->layout.target_dir;

  // PRD §12.1: never remove shared caches, dependencies or toolchains by default.
  if (!fs::exists(target_dir)) {
    ctx.ui->info("nothing to clean: " + display_path(target_dir, project->layout.root) +
                 " does not exist");
    return 0;
  }

  std::vector<fs::Path> removable;
  if (clean_all) {
    removable.push_back(target_dir);
  } else if (ctx.flags.boolean("release") || ctx.flags.get("profile")) {
    // Only the selected profile subtree of each target.
    const std::string profile = selected_profile(ctx);
    std::error_code ec;
    for (auto it = stdfs::directory_iterator(target_dir, ec); it != stdfs::directory_iterator();
         it.increment(ec)) {
      if (!fs::is_directory(it->path())) {
        continue;
      }
      const auto profile_dir = it->path() / profile;
      if (fs::is_directory(profile_dir)) {
        removable.push_back(profile_dir);
      }
    }
  } else {
    removable.push_back(target_dir);
  }

  if (ctx.global.target && !ctx.global.target->empty()) {
    auto resolved = platform::resolve_target(*ctx.global.target);
    if (!resolved) {
      return report_error(resolved.error(), ctx);
    }
    const auto component = target_dir_component(*resolved);
    std::vector<fs::Path> narrowed;
    for (const auto& path : removable) {
      if (path == target_dir) {
        const auto candidate = target_dir / component;
        if (fs::exists(candidate)) {
          narrowed.push_back(candidate);
        }
      } else if (fs::generic_string(path).find(component) != std::string::npos) {
        narrowed.push_back(path);
      }
    }
    removable = std::move(narrowed);
  }

  if (removable.empty()) {
    ctx.ui->info("nothing to clean");
    return 0;
  }

  std::uint64_t total_bytes = 0;
  for (const auto& path : removable) {
    std::error_code ec;
    if (!stdfs::is_directory(path, ec)) {
      continue;
    }
    for (auto it = stdfs::recursive_directory_iterator(path, ec);
         it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
      std::error_code size_ec;
      if (it->is_regular_file(size_ec)) {
        total_bytes += it->file_size(size_ec);
      }
    }
  }

  for (const auto& path : removable) {
    if (dry_run) {
      ctx.ui->out("  would remove " + display_path(path, project->layout.root));
      continue;
    }
    // The allowed root is the project's target directory: cleaning can never
    // reach into ~/.cova or the source tree (PRD §12.4).
    // The allowed root is the project root, so cleaning may remove the whole
    // target directory (but never anything outside it). The per-path check
    // below keeps the operation scoped to `target/`.
    if (!fs::is_within(path, target_dir)) {
      return report_error(Error::make("cova::clean::outside_target",
                                      "refusing to clean a path outside target/"),
                          ctx);
    }
    auto removed = fs::remove_tree(path, project->layout.root);
    if (!removed) {
      return report_error(removed.error(), ctx);
    }
    ctx.ui->out("  removed " + display_path(path, project->layout.root) + " (" +
                human_bytes(total_bytes) + ")");
  }

  if (dry_run) {
    ctx.ui->info("would remove " + human_bytes(total_bytes));
  } else {
    ctx.ui->info("cleaned " + human_bytes(total_bytes) + " of build artifacts");
    ctx.ui->verbose("shared caches, dependencies and toolchains were left untouched");
  }
  return 0;
}

int cmd_graph(CommandContext& ctx) {
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  auto target = resolve_target_from(ctx, *project);
  if (!target) {
    return report_error(target.error(), ctx);
  }
  auto config = effective_config(ctx);
  if (!config) {
    return report_error(config.error(), ctx);
  }
  auto request = make_resolution_request(ctx, *project, *target, *config);
  auto resolved = resolve_toolchain_or_report(ctx, request);
  if (!resolved) {
    return 1;
  }
  auto plan = plan_for(ctx, *project, *resolved, *target, /*include_tests=*/true);
  if (!plan) {
    return report_error(plan.error(), ctx);
  }

  const std::string format = ctx.flags.get("format").value_or("text");
  if (format == "dot") {
    ctx.ui->out(plan->graph.to_dot());
    return 0;
  }
  if (format == "json") {
    json::Value root = json::Value::object();
    root.set("root", fs::generic_string(project->layout.root));
    root.set("target", target->to_string());
    auto actions = json::Value::array();
    for (const auto& action : plan->graph.actions_in_order()) {
      json::Value entry = json::Value::object();
      entry.set("id", action->id.value);
      entry.set("kind", std::string(build::to_string(action->kind)));
      entry.set("label", action->label);
      entry.set("package", action->package);
      entry.set("exec_platform", std::string(build::to_string(action->exec_platform)));
      auto inputs = json::Value::array();
      for (const auto& input : action->inputs) {
        inputs.push(fs::generic_string(input.path));
      }
      entry.set("inputs", std::move(inputs));
      auto outputs = json::Value::array();
      for (const auto& output : action->outputs) {
        outputs.push(fs::generic_string(output.path));
      }
      entry.set("outputs", std::move(outputs));
      auto deps = json::Value::array();
      for (const auto& dep : action->dependencies) {
        deps.push(dep.value);
      }
      entry.set("dependencies", std::move(deps));
      actions.push(std::move(entry));
    }
    root.set("actions", std::move(actions));
    ctx.ui->out(root.dump(2));
    return 0;
  }

  ctx.ui->out(plan->graph.to_text());
  return 0;
}

int cmd_explain(CommandContext& ctx) {
  auto project = load_project(ctx);
  std::optional<LoadedProject> standalone;
  if (!project) {
    // `cova toolchain explain` is a toolchain-management command and must work
    // outside a project. Explain the host environment with default settings.
    LoadedProject fallback;
    fallback.layout = layout_for(ctx.cwd);
    fallback.manifest = manifest::default_manifest("cova", manifest::PackageKind::Executable);
    fallback.manifest.root = ctx.cwd;
    standalone = std::move(fallback);
    project = *standalone;
  }
  auto target = resolve_target_from(ctx, *project);
  if (!target) {
    return report_error(target.error(), ctx);
  }
  auto config = effective_config(ctx);
  if (!config) {
    return report_error(config.error(), ctx);
  }

  toolchain::ResolutionRequest request = make_resolution_request(ctx, *project, *target, *config);
  request.explain = true;
  auto report = toolchain::resolve(request);

  if (ctx.global.json) {
    json::Value root = json::Value::object();
    root.set("target", target->to_string());
    root.set("resolved", report.resolved);
    auto decisions = json::Value::array();
    for (const auto& decision : report.decisions) {
      decisions.push(decision);
    }
    root.set("decisions", std::move(decisions));
    auto rejected = json::Value::array();
    for (const auto& candidate : report.rejected) {
      json::Value entry = json::Value::object();
      entry.set("toolchain", candidate.toolchain_label);
      entry.set("reason", std::string(platform::to_string(candidate.reason)));
      entry.set("explanation", candidate.explanation);
      rejected.push(std::move(entry));
    }
    root.set("rejected", std::move(rejected));
    if (report.resolved) {
      root.set("selected", report.toolchain.descriptor.label());
      root.set("fingerprint", report.toolchain.fingerprint);
    }
    ctx.ui->out(root.dump(2));
    return report.resolved ? 0 : 1;
  }

  ctx.ui->out("target: " + target->to_string());
  ctx.ui->out("policy: " + std::string(toolchain::to_string(request.policy)));
  if (!ctx.global.quiet) {
    ctx.ui->out("");
    ctx.ui->out("resolution chain:");
    for (const auto& decision : report.decisions) {
      ctx.ui->out("  - " + decision);
    }
    if (!report.rejected.empty()) {
      ctx.ui->out("");
      ctx.ui->out("rejected candidates:");
      for (const auto& candidate : report.rejected) {
        ctx.ui->out("  - " + candidate.toolchain_label + ": " +
                    std::string(platform::to_string(candidate.reason)));
        if (!candidate.explanation.empty()) {
          ctx.ui->out("    " + candidate.explanation);
        }
      }
    }
  }
  if (report.resolved) {
    ctx.ui->out("");
    ctx.ui->out("selected: " + report.toolchain.descriptor.label());
    auto& tc = report.toolchain;
    if (tc.descriptor.component("cxx") != nullptr) {
      ctx.ui->out("  c++ driver: " + fs::generic_string(tc.descriptor.component_path("cxx")));
    }
    if (tc.descriptor.component("linker") != nullptr) {
      ctx.ui->out("  linker: " + fs::generic_string(tc.descriptor.component_path("linker")));
    }
    if (!tc.sysroot_path.empty()) {
      ctx.ui->out("  sysroot: " + fs::generic_string(tc.sysroot_path));
    }
    ctx.ui->out("  fingerprint: " + tc.fingerprint);
    return 0;
  }
  DiagnosticEngine engine;
  engine.emit(report.failure);
  return report_diagnostics(engine, ctx);
}

int cmd_metadata(CommandContext& ctx) {
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  auto target = resolve_target_from(ctx, *project);
  if (!target) {
    return report_error(target.error(), ctx);
  }

  json::Value root = json::Value::object();
  root.set("manifest_path", fs::generic_string(project->layout.manifest));
  root.set("root", fs::generic_string(project->layout.root));
  root.set("target_dir", fs::generic_string(project->layout.target_dir));
  root.set("target", target->to_string());
  root.set("profile", selected_profile(ctx));

  json::Value package = json::Value::object();
  package.set("name", project->manifest.package.name);
  package.set("version", project->manifest.package.version);
  package.set("edition", project->manifest.package.edition);
  package.set("kind", std::string(manifest::to_string(project->manifest.build.kind)));
  root.set("package", std::move(package));

  auto sources = json::Value::array();
  for (const auto& pattern : project->manifest.build.sources) {
    sources.push(pattern);
  }
  root.set("sources", std::move(sources));

  auto deps = json::Value::object();
  for (const auto& dependency : project->manifest.dependencies) {
    json::Value entry = json::Value::object();
    entry.set("requirement", dependency.requirement_text);
    if (dependency.path) {
      entry.set("path", fs::generic_string(*dependency.path));
    }
    if (dependency.git) {
      entry.set("git", *dependency.git);
    }
    if (!dependency.features.empty()) {
      auto features = json::Value::array();
      for (const auto& feature : dependency.features) {
        features.push(feature);
      }
      entry.set("features", std::move(features));
    }
    deps.set(dependency.name, std::move(entry));
  }
  root.set("dependencies", std::move(deps));

  bool is_ws = project->workspace.has_value();
  root.set("workspace", is_ws);
  if (is_ws) {
    auto members = json::Value::array();
    for (const auto& member : project->workspace->members) {
      members.push(member.name);
    }
    root.set("members", std::move(members));
  }

  auto toolchain = json::Value::object();
  toolchain.set("policy", std::string(toolchain::to_string(project->manifest.toolchain.policy)));
  toolchain.set("compiler", project->manifest.toolchain.compiler);
  toolchain.set("version", project->manifest.toolchain.version);
  toolchain.set("auto_install", project->manifest.toolchain.auto_install);
  root.set("toolchain", std::move(toolchain));

  ctx.ui->out(root.dump(2));
  return 0;
}

}  // namespace cova::cli
