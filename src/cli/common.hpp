// Cova — shared command plumbing.
//
// Every command needs the same three things: a loaded project, a frozen
// compilation environment, and a build plan. This header holds that pipeline
// once so individual commands stay small.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "build/planner.hpp"
#include "cli/cli.hpp"
#include "core/config/config.hpp"
#include "core/diagnostics/diagnostic.hpp"
#include "manifest/manifest.hpp"
#include "package/dependency/dependency.hpp"
#include "package/lockfile/lockfile.hpp"
#include "platform/triple/triple.hpp"
#include "toolchain/descriptor.hpp"
#include "toolchain/resolver/resolver.hpp"
#include "workspace/workspace.hpp"

namespace cova::cli {

namespace stdfs = std::filesystem;

// Directories Cova uses inside a project.
struct ProjectLayout {
  stdfs::path root;        // project (or workspace) root
  stdfs::path target_dir;  // root/target
  stdfs::path manifest;    // root/cova.toml
  stdfs::path lockfile;    // root/cova.lock
  stdfs::path toolchain_lock;  // root/cova-toolchain.lock
  stdfs::path vendor_dir;  // root/vendor
};

// A loaded project: manifest plus workspace information, resolved once.
struct LoadedProject {
  ProjectLayout layout;
  manifest::Manifest manifest;            // the package to act on
  std::optional<workspace::Workspace> workspace;  // set when inside a workspace
  bool is_workspace_root = false;
  std::vector<std::string> warnings;
};

// Locates the project from the current directory, honouring --manifest-path.
Result<LoadedProject> load_project(CommandContext& ctx);

// Computes the layout for a root directory without loading anything.
ProjectLayout layout_for(const stdfs::path& root);

// Resolves the target triple from CLI --target, else the manifest, else host.
Result<platform::Triple> resolve_target_from(CommandContext& ctx, const LoadedProject& project);

// Builds a ResolutionRequest from the CLI + manifest state.
toolchain::ResolutionRequest make_resolution_request(CommandContext& ctx,
                                                     const LoadedProject& project,
                                                     const platform::Triple& target,
                                                     const config::EffectiveConfig& config);

// Resolves the toolchain and reports failures as a rendered diagnostic.
// Returns nullopt after printing the diagnostic, so callers return an exit code.
std::optional<toolchain::ResolvedToolchain> resolve_toolchain_or_report(
    CommandContext& ctx, const toolchain::ResolutionRequest& request);

// Loads (or creates) the toolchain lockfile and pins the resolution to it in
// locked mode. `update` is set when the lock should be rewritten after success.
struct LockState {
  lockfile::ToolchainLock lock;
  bool present = false;
  bool should_write = false;
};
Result<LockState> load_toolchain_lock(const LoadedProject& project, CommandContext& ctx);
Status store_toolchain_lock(const LoadedProject& project, const LockState& state,
                            const toolchain::ResolvedToolchain& resolved);

// The effective configuration for this invocation.
Result<config::EffectiveConfig> effective_config(CommandContext& ctx);

// Local dependency resolution plus the shared `cova.lock`.
struct DependencyState {
  package::DependencyGraph graph;
  // Packages to build, dependency-first: the selected roots and everything they
  // reach through target-active dependency edges.
  std::vector<manifest::Manifest> build_packages;
  std::vector<std::string> roots;
  stdfs::path lockfile;
  bool lock_written = false;
  bool lock_up_to_date = false;
};

// Resolves every local path dependency reachable from the selected roots.
// `apply_lock` enforces `--locked` and writes `cova.lock` when it is stale;
// read-only callers (`cova graph`) pass false and never touch the file.
Result<DependencyState> resolve_dependencies(CommandContext& ctx, const LoadedProject& project,
                                             const platform::Triple& target, bool apply_lock,
                                             bool include_dev);

// Plans the build for this invocation. `include_tests` controls whether test
// actions are part of the graph.
Result<build::PlanResult> plan_for(CommandContext& ctx, const LoadedProject& project,
                                   const toolchain::ResolvedToolchain& tc,
                                   const platform::Triple& target, bool include_tests);

// Plans a resolved dependency graph. The toolchain is already frozen.
Result<build::PlanResult> plan_with_dependencies(CommandContext& ctx, const LoadedProject& project,
                                                 const DependencyState& dependencies,
                                                 const toolchain::ResolvedToolchain& tc,
                                                 const platform::Triple& target, bool include_tests);

// Writes the compilation database for a plan. Returns the path written.
Result<stdfs::path> write_compile_commands(const LoadedProject& project,
                                           const build::PlanResult& plan);

// Profile name selected by CLI flags (--release wins over --profile).
std::string selected_profile(const CommandContext& ctx);

// Directory name for a target inside `target/`.
std::string target_dir_component(const platform::Triple& target);

// Prints a diagnostic engine's contents and returns a conventional exit code.
int report_diagnostics(const DiagnosticEngine& engine, CommandContext& ctx);

// Prints an error as a one-line diagnostic.
int report_error(const Error& error, CommandContext& ctx);

// Renders and prints a ResolutionReport failure.
void print_resolution_failure(const toolchain::ResolutionReport& report, CommandContext& ctx);

// Formats a duration for status output ("1.24s", "340ms").
std::string format_duration(std::chrono::milliseconds duration);

// Formats a path relative to `base` when possible, for tidy output.
std::string display_path(const stdfs::path& path, const stdfs::path& base);

// True when the terminal attached to stdout is interactive.
bool stdout_is_tty();

// True when the terminal attached to stderr is interactive. Progress output
// uses stderr, so this is what decides whether it can redraw in place.
bool stderr_is_tty();

// Width in columns of the terminal on stderr, or 80 when it cannot be queried.
int terminal_width();

}  // namespace cova::cli
