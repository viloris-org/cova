// Cova — shared command plumbing implementation.
#include "cli/common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "core/diagnostics/diagnostic.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/process/process.hpp"

namespace cova::cli {
namespace {

// Splits "clang@>=20, <23" into family and requirement.
void split_compiler_spec(std::string_view spec, std::string& family, std::string& version) {
  auto at = spec.find('@');
  if (at == std::string_view::npos) {
    family = std::string(spec);
    version.clear();
    return;
  }
  family = std::string(spec.substr(0, at));
  version = std::string(spec.substr(at + 1));
}

}  // namespace

bool stdout_is_tty() { return ::isatty(STDOUT_FILENO) != 0; }

std::string format_duration(std::chrono::milliseconds duration) {
  const auto ms = duration.count();
  if (ms < 1000) {
    return std::to_string(ms) + "ms";
  }
  const double seconds = static_cast<double>(ms) / 1000.0;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2fs", seconds);
  return std::string(buffer);
}

std::string display_path(const stdfs::path& path, const stdfs::path& base) {
  if (base.empty()) {
    return fs::generic_string(path);
  }
  std::error_code ec;
  auto relative = stdfs::relative(path, base, ec);
  if (ec || relative.empty()) {
    return fs::generic_string(path);
  }
  const std::string text = fs::generic_string(relative);
  if (text.rfind("..", 0) == 0) {
    return fs::generic_string(path);
  }
  return text;
}

ProjectLayout layout_for(const stdfs::path& root) {
  ProjectLayout layout;
  layout.root = root;
  layout.target_dir = root / "target";
  layout.manifest = root / "cova.toml";
  layout.lockfile = root / "cova.lock";
  layout.toolchain_lock = root / "cova-toolchain.lock";
  layout.vendor_dir = root / "vendor";
  return layout;
}

std::string target_dir_component(const platform::Triple& target) {
  // Directory-safe encoding: vendor is dropped, remaining components joined.
  std::string text = target.to_short_string();
  std::replace(text.begin(), text.end(), '/', '-');
  return text;
}

std::string selected_profile(const CommandContext& ctx) {
  if (ctx.flags.boolean("release")) {
    return "release";
  }
  if (auto explicit_profile = ctx.flags.get("profile")) {
    if (!explicit_profile->empty()) {
      return *explicit_profile;
    }
  }
  return ctx.global.profile.empty() ? "dev" : ctx.global.profile;
}

Result<config::EffectiveConfig> effective_config(CommandContext& ctx) {
  auto dirs = fs::cova_dirs();
  if (!dirs) {
    return fail<config::EffectiveConfig>(dirs.error());
  }
  auto user = config::load_user_config();
  if (!user) {
    return fail<config::EffectiveConfig>(user.error());
  }
  std::map<std::string, std::string, std::less<>> cli_flags;
  cli_flags["offline"] = ctx.global.offline_mode() ? "1" : "0";
  cli_flags["locked"] = ctx.global.locked_mode() ? "1" : "0";
  cli_flags["frozen"] = ctx.global.frozen ? "1" : "0";
  cli_flags["no-install"] = ctx.global.no_install ? "1" : "0";
  cli_flags["verbose"] = ctx.global.verbose ? "1" : "0";
  cli_flags["quiet"] = ctx.global.quiet ? "1" : "0";
  cli_flags["jobs"] = std::to_string(ctx.global.jobs);
  cli_flags["color"] = ctx.global.color;
  return config::merge(*user, dirs->root, cli_flags);
}

Result<LoadedProject> load_project(CommandContext& ctx) {
  LoadedProject loaded;

  // Honour --manifest-path.
  stdfs::path manifest_path;
  if (auto explicit_path = ctx.flags.get("manifest-path")) {
    manifest_path = fs::absolute_normalized(*explicit_path);
    if (!fs::is_regular_file(manifest_path)) {
      return fail<LoadedProject>(
          Error::make("cova::cli::manifest_not_found",
                      "no manifest at " + fs::generic_string(manifest_path)));
    }
  } else {
    auto found = manifest::find_manifest_upwards(ctx.cwd);
    if (!found) {
      return fail<LoadedProject>(
          Error::make("cova::cli::not_a_package",
                      "could not find `cova.toml` in " + fs::generic_string(ctx.cwd) +
                          " or any parent directory")
              .with("run `cova new <name>` to create a project, or `cova init` in an "
                    "existing one"));
    }
    manifest_path = *found;
  }

  auto root = manifest_path.parent_path();
  loaded.layout = layout_for(root);
  if (auto explicit_target_dir = ctx.flags.get("target-dir")) {
    loaded.layout.target_dir = fs::absolute_normalized(*explicit_target_dir);
  }
  ctx.project_root = root;

  auto loaded_manifest = manifest::load(manifest_path);
  if (!loaded_manifest) {
    return fail<LoadedProject>(loaded_manifest.error());
  }
  loaded.manifest = std::move(loaded_manifest->manifest);
  loaded.warnings = std::move(loaded_manifest->warnings);
  loaded.is_workspace_root = loaded.manifest.is_workspace_root();

  // Attach workspace information. When the manifest itself is a workspace root,
  // load it; when it is a plain package, look upward for a workspace that
  // contains it.
  if (loaded.is_workspace_root) {
    auto ws = workspace::load(root);
    if (!ws) {
      return fail<LoadedProject>(ws.error());
    }
    loaded.workspace = std::move(*ws);
  } else {
    stdfs::path probe = root.parent_path();
    bool found = false;
    while (!probe.empty() && probe != probe.parent_path()) {
      auto candidate = probe / "cova.toml";
      if (fs::is_regular_file(candidate)) {
        auto ws_manifest = manifest::load(candidate);
        if (ws_manifest && ws_manifest->manifest.is_workspace_root()) {
          auto ws = workspace::load(probe);
          if (ws) {
            loaded.workspace = std::move(*ws);
            loaded.is_workspace_root = false;
            found = true;
          }
          break;
        }
      }
      probe = probe.parent_path();
    }
    (void)found;
  }

  return loaded;
}

Result<platform::Triple> resolve_target_from(CommandContext& ctx, const LoadedProject& project) {
  if (ctx.global.target && !ctx.global.target->empty()) {
    auto resolved = platform::resolve_target(*ctx.global.target);
    if (!resolved) {
      return fail<platform::Triple>(resolved.error());
    }
    return *resolved;
  }
  if (auto declared = project.manifest.declared_target()) {
    return *declared;
  }
  return platform::host_triple();
}

toolchain::ResolutionRequest make_resolution_request(CommandContext& ctx,
                                                     const LoadedProject& project,
                                                     const platform::Triple& target,
                                                     const config::EffectiveConfig& config) {
  toolchain::ResolutionRequest request;
  request.target = target;

  const auto& toolchain_section = project.manifest.toolchain;
  request.policy = toolchain_section.policy;
  request.compiler_family = toolchain_section.compiler.empty()
                                ? std::nullopt
                                : std::optional<std::string>(toolchain_section.compiler);
  request.compiler_version = toolchain_section.version.empty()
                                 ? std::nullopt
                                 : std::optional<std::string>(toolchain_section.version);
  request.constraints = project.manifest.constraints;
  request.locked = config.locked;
  request.offline = config.offline;
  request.allow_install = config.allow_install && toolchain_section.auto_install;
  request.explain = ctx.global.verbose;

  // CLI overrides win over manifest declarations (§5.4).
  if (ctx.global.compiler) {
    std::string family;
    std::string version;
    split_compiler_spec(*ctx.global.compiler, family, version);
    if (!family.empty()) {
      request.compiler_family = family;
    }
    if (!version.empty()) {
      request.compiler_version = version;
    }
  }
  if (ctx.global.toolchain) {
    request.pinned_id = *ctx.global.toolchain;
  } else if (!toolchain_section.preferred.empty()) {
    request.pinned_id = toolchain_section.preferred;
  }

  auto dirs = fs::cova_dirs();
  if (dirs) {
    request.toolchains_root = dirs->toolchains;
    request.sysroots_root = dirs->sysroots;
    request.store_root = dirs->store;
    request.cache_root = dirs->cache;
  }
  if (auto sysroot = ctx.flags.get("sysroot"); sysroot && !sysroot->empty()) {
    // An explicit sysroot is a hard constraint on the resolved environment.
    platform::Constraint constraint;
    constraint.kind = platform::ConstraintKind::SdkVersion;
    constraint.key = "sysroot";
    constraint.value = *sysroot;
    constraint.origin = "cli";
    request.constraints.push_back(std::move(constraint));
  }
  request.probe_capabilities = !ctx.dry_run;
  return request;
}

std::optional<toolchain::ResolvedToolchain> resolve_toolchain_or_report(
    CommandContext& ctx, const toolchain::ResolutionRequest& request) {
  auto report = toolchain::resolve(request);
  if (!report.resolved) {
    print_resolution_failure(report, ctx);
    return std::nullopt;
  }
  if (ctx.global.verbose) {
    for (const auto& decision : report.decisions) {
      ctx.ui->verbose(decision);
    }
  }
  return report.toolchain;
}

namespace {

void render_diagnostic(const Diagnostic& diagnostic, CommandContext& ctx, int depth = 0) {
  const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
  switch (diagnostic.severity) {
    case Severity::Warning:
      ctx.ui->warn(indent + diagnostic.message);
      break;
    case Severity::Error:
    case Severity::Bug:
      ctx.ui->error(indent + diagnostic.message);
      break;
    case Severity::Help:
    case Severity::Note:
      ctx.ui->out(indent + diagnostic.message);
      break;
  }
  if (!diagnostic.detail.empty()) {
    for (const auto& line : str::split(diagnostic.detail, '\n')) {
      if (!line.empty()) {
        ctx.ui->out(indent + "  " + line);
      }
    }
  }
  for (const auto& child : diagnostic.children) {
    render_diagnostic(child, ctx, depth + 1);
  }
}

}  // namespace

void print_resolution_failure(const toolchain::ResolutionReport& report, CommandContext& ctx) {
  DiagnosticEngine engine;
  if (report.failure.code.empty()) {
    engine.emit(make_error("cova::toolchain::unresolved", "unable to resolve a toolchain"));
  } else {
    engine.emit(report.failure);
  }
  report_diagnostics(engine, ctx);
  for (const auto& decision : report.decisions) {
    ctx.ui->verbose(decision);
  }
}

int report_diagnostics(const DiagnosticEngine& engine, CommandContext& ctx) {
  for (const auto& diagnostic : engine.diagnostics()) {
    render_diagnostic(diagnostic, ctx);
  }
  if (!engine.has_errors()) {
    return 0;
  }
  const std::string summary = engine.summary();
  if (!summary.empty()) {
    ctx.ui->error(summary);
  }
  return 1;
}

int report_error(const Error& error, CommandContext& ctx) {
  DiagnosticEngine engine;
  engine.emit(diagnostic_from_error(error));
  return report_diagnostics(engine, ctx);
}

Result<LockState> load_toolchain_lock(const LoadedProject& project, CommandContext& ctx) {
  LockState state;
  const auto& path = project.layout.toolchain_lock;
  if (!fs::is_regular_file(path)) {
    if (ctx.global.locked_mode()) {
      return fail<LockState>(
          Error::make("cova::lock::missing",
                      "no `cova-toolchain.lock` found but locked mode was requested")
              .with("path: " + fs::generic_string(path))
              .with("run without --locked to resolve and write a lockfile"));
    }
    state.should_write = true;
    return state;
  }
  auto loaded = lockfile::ToolchainLock::load(path);
  if (!loaded) {
    return fail<LockState>(loaded.error());
  }
  state.lock = std::move(*loaded);
  state.present = true;
  return state;
}

Status store_toolchain_lock(const LoadedProject& project, const LockState& state,
                            const toolchain::ResolvedToolchain& resolved) {
  lockfile::ToolchainLock lock = state.lock;
  if (lock.version.empty()) {
    lock.version = "1";
  }
  lock.generated_by = version_text();

  auto entry = lockfile::lock_entry_from(resolved);
  bool replaced = false;
  for (auto& existing : lock.entries) {
    if (existing.target == entry.target) {
      existing = entry;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    lock.entries.push_back(entry);
  }
  std::sort(lock.entries.begin(), lock.entries.end(),
            [](const lockfile::LockedToolchain& a, const lockfile::LockedToolchain& b) {
              return a.target < b.target;
            });
  return lock.save(project.layout.toolchain_lock);
}

Result<build::PlanResult> plan_for(CommandContext& ctx, const LoadedProject& project,
                                   const toolchain::ResolvedToolchain& tc,
                                   const platform::Triple& target, bool include_tests) {
  build::PlanOptions options;
  options.target = target;
  options.profile = selected_profile(ctx);
  options.jobs = ctx.global.jobs;
  options.use_color = ctx.global.use_color(stdout_is_tty());
  options.include_tests = include_tests;
  options.explain = ctx.global.verbose;
  options.workspace = ctx.flags.boolean("workspace") || project.is_workspace_root;

  if (project.workspace && options.workspace) {
    build::WorkspaceProject ws_project;
    ws_project.root_manifest = project.workspace->root;
    for (const auto& member : project.workspace->members) {
      auto member_manifest = manifest::load(member.root / "cova.toml");
      if (!member_manifest) {
        return fail<build::PlanResult>(member_manifest.error());
      }
      ws_project.members.push_back(std::move(member_manifest->manifest));
    }
    return build::plan_workspace(ws_project, tc, options, project.layout.target_dir);
  }
  return build::plan_package(project.manifest, tc, options, project.layout.target_dir);
}

}  // namespace cova::cli
