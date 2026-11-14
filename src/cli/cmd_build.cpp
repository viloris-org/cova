// Cova — `cova build`, `cova check`, `cova run`, `cova test` (PRD §9, §13).
//
// These four commands share one pipeline:
//   load project → resolve target → resolve toolchain (frozen) → plan graph
//   → restore fresh outputs from cache → execute → report
// Keeping it in one file makes the shared preconditions impossible to get wrong
// in one command but right in another.
#include <algorithm>
#include <chrono>
#include <atomic>
#include <iostream>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "build/cache/cache.hpp"
#include "build/executor/executor.hpp"
#include "build/planner.hpp"
#include "build/scheduler/scheduler.hpp"
#include "cli/common.hpp"
#include "cli/progress.hpp"
#include "core/diagnostics/diagnostic.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/json/json.hpp"
#include "core/process/environment.hpp"
#include "core/process/process.hpp"
#include "platform/binary/binary.hpp"

namespace cova::cli {
namespace {

using Clock = std::chrono::steady_clock;

// Status/progress is for humans: it is suppressed by --quiet and by --json,
// where stdout must remain a machine-readable document and stderr should stay
// quiet unless something is wrong.
bool status_enabled(const CommandContext& ctx) {
  return !ctx.global.quiet && !ctx.global.json;
}

std::string elapsed_ms(Clock::time_point started) {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - started)
                            .count());
}

std::string project_label(const LoadedProject& project) {
  if (!project.manifest.package.name.empty()) {
    return project.manifest.package.name;
  }
  if (project.workspace) {
    const std::string name = project.workspace->root_dir.filename().string();
    if (!name.empty()) {
      return name;
    }
  }
  return "workspace";
}

// A fully prepared build: everything resolved and planned, nothing executed.
struct PreparedBuild {
  LoadedProject project;
  platform::Triple target;
  toolchain::ResolvedToolchain toolchain;
  build::PlanResult plan;
  LockState lock_state;
};

// Builds the explicit child environment every action receives. The environment
// is part of the action identity, and an untracked environment makes the whole
// build ineligible for cross-execution reuse.
Result<build::ActionEnvironment> make_action_environment(const PreparedBuild& prepared,
                                                         CommandContext& ctx) {
  build::ActionEnvironment environment;
  environment.target = prepared.target;
  environment.abi = prepared.toolchain.abi_signature;
  environment.toolchain_fingerprint = prepared.toolchain.fingerprint;
  environment.sysroot_fingerprint = prepared.toolchain.sysroot_fingerprint.empty()
                                        ? prepared.toolchain.sysroot_id
                                        : prepared.toolchain.sysroot_fingerprint;

  proc::EnvironmentRequest request;
  request.inherit_untracked = ctx.flags.boolean("inherit-env");
  auto snapshot = proc::collect_environment(request);
  if (!snapshot) {
    return std::unexpected(std::move(snapshot.error()));
  }
  environment.environment = snapshot->as_map();
  environment.explicit_environment = true;
  environment.isolation = snapshot->isolation;
  environment.shared_cache_eligible = snapshot->shared_cache_eligible;
  environment.incremental_eligible = snapshot->incremental_eligible;
  if (!environment.shared_cache_eligible && ctx.global.verbose) {
    ctx.ui->verbose(std::string("environment is ") + std::string(proc::to_string(environment.isolation)) +
                    "; shared artifact reuse is disabled");
  }
  return environment;
}

// Asks the compiler for the translation unit's dependency closure before the
// action runs. This is the provider-side discovery step: it catches a stale
// depfile, a newly added higher-priority header, and a conditional include that
// the previous depfile could not know about. The resulting header set is part
// of the final action identity.
Result<bool> refresh_compile_closure(const build::Action& action,
                                     const build::ActionEnvironment& environment,
                                     std::string& detail) {
  if (action.kind != build::ActionKind::Compile || action.argv.empty()) {
    return true;
  }
  auto& mutable_action = const_cast<build::Action&>(action);
  mutable_action.input_closure_checked = true;
  mutable_action.input_closure_scan_failed = false;
  auto scratch = fs::make_temp_dir(std::filesystem::temp_directory_path(), "cova-scan-");
  if (!scratch) {
    return fail<bool>(scratch.error());
  }
  const auto depfile = *scratch / "dependencies.d";
  std::vector<std::string> argv;
  argv.push_back(action.argv.front());
  for (std::size_t index = 1; index < action.argv.size(); ++index) {
    const std::string& argument = action.argv[index];
    if (argument == "-MMD" || argument == "-MD" || argument == "-c") {
      continue;
    }
    if (argument == "-MF" || argument == "-o") {
      if (index + 1 < action.argv.size()) {
        ++index;
      }
      continue;
    }
    if (argument.rfind("-MF", 0) == 0) {
      continue;
    }
    argv.push_back(argument);
  }
  argv.push_back("-MM");
  argv.push_back("-MF");
  argv.push_back(fs::generic_string(depfile));

  proc::ProcessSpec spec;
  spec.program = argv.front();
  spec.args.assign(argv.begin() + 1, argv.end());
  spec.cwd = action.working_dir;
  if (environment.explicit_environment) {
    std::vector<std::pair<std::string, std::string>> variables;
    variables.reserve(environment.environment.size());
    for (const auto& [name, value] : environment.environment) {
      variables.emplace_back(name, value);
    }
    spec.environment = std::move(variables);
  }
  auto result = proc::run(spec);
  if (!result || !result->success()) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    mutable_action.input_closure_complete = false;
    mutable_action.input_closure_scan_failed = true;
    Error error = Error::make("cova::build::dependency_scan",
                              "the compiler could not scan the input closure");
    if (result && !result->stderr_text.empty()) {
      error.with(str::trim(result->stderr_text));
    } else if (!result) {
      error.with(result.error().message);
    }
    return fail<bool>(std::move(error));
  }
  auto parsed = build::Depfile::load(depfile, action.working_dir);
  auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
  (void)cleanup;
  if (!parsed) {
    mutable_action.input_closure_complete = false;
    mutable_action.input_closure_scan_failed = true;
    return fail<bool>(Error::make("cova::build::dependency_scan",
                                  "the compiler did not produce a readable dependency file"));
  }
  // The scan is the authority on the discovered closure. Drop headers learned
  // from an older depfile (including another target's configuration) so stale
  // discoveries cannot accumulate in the identity.
  mutable_action.inputs.erase(
      std::remove_if(mutable_action.inputs.begin(), mutable_action.inputs.end(),
                     [](const build::FileInput& input) {
                       return input.origin != build::InputOrigin::Declared;
                     }),
      mutable_action.inputs.end());
  std::set<std::string> known;
  for (const auto& input : mutable_action.inputs) {
    known.insert(fs::generic_string(fs::canonical_for_compare(input.path)));
  }
  for (const auto& dependency : parsed->dependencies) {
    if (!fs::is_regular_file(dependency)) {
      continue;
    }
    const std::string key = fs::generic_string(fs::canonical_for_compare(dependency));
    if (known.insert(key).second) {
      mutable_action.inputs.push_back(
          build::FileInput{dependency, "header", build::InputOrigin::Discovered});
    }
  }
  mutable_action.input_closure_complete = true;
  // Publish the fresh closure at the provider's canonical depfile path. The
  // next planning pass reads it before deciding whether anything is stale.
  if (!mutable_action.outputs.empty()) {
    fs::Path canonical_depfile = mutable_action.outputs.front().path;
    canonical_depfile += ".d";
    auto copied = fs::copy_file(depfile, canonical_depfile, true);
    (void)copied;
  }
  mutable_action.cache_key(environment);
  detail = "input closure refreshed by a compiler dependency scan";
  return true;
}

bool cache_has_entries(const build::CacheSettings& settings) {
  if (!settings.enabled) {
    return false;
  }
  std::error_code ec;
  const auto actions = settings.root / "actions";
  if (!std::filesystem::is_directory(actions, ec)) {
    return false;
  }
  for (auto it = std::filesystem::directory_iterator(actions, ec);
       it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (ec) {
      return true;  // Do not claim "cold" when the cache cannot be inspected.
    }
    return true;
  }
  return false;
}

// On a genuinely cold build there is no state or cache to consult, and every
// compile action needs its closure. Run those scans concurrently instead of
// serializing them behind the executor's up-to-date mutex.
void run_cold_dependency_scans(const build::BuildGraph& graph,
                               const build::ActionEnvironment& environment,
                               int requested_workers, CommandContext& ctx) {
  std::vector<const build::Action*> actions;
  for (const build::Action* action : graph.actions_in_order()) {
    if (action->kind != build::ActionKind::Compile) {
      continue;
    }
    // A compile that depends on another action may consume generated sources
    // or headers. Scanning it before its dependency has run would be wrong;
    // fall back to the executor's dependency-ordered checks.
    if (!action->dependencies.empty()) {
      return;
    }
    if (!action->input_closure_checked) {
      actions.push_back(action);
    }
  }
  if (actions.empty()) {
    return;
  }
  const int hardware = static_cast<int>(std::thread::hardware_concurrency());
  const int workers = std::max(
      1, std::min<int>(requested_workers > 0 ? requested_workers : hardware,
                       static_cast<int>(actions.size())));
  struct ScanResult {
    bool succeeded = false;
    long long milliseconds = 0;
  };
  std::vector<ScanResult> results(actions.size());
  std::atomic<std::size_t> next{0};
  auto worker = [&]() {
    for (;;) {
      const std::size_t index = next.fetch_add(1);
      if (index >= actions.size()) {
        return;
      }
      const auto started = Clock::now();
      std::string detail;
      auto refreshed = refresh_compile_closure(*actions[index], environment, detail);
      results[index].succeeded = refreshed.has_value() && *refreshed;
      results[index].milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        Clock::now() - started)
                                        .count();
    }
  };
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(workers));
  for (int index = 0; index < workers; ++index) {
    threads.emplace_back(worker);
  }
  for (auto& thread : threads) {
    thread.join();
  }
  if (ctx.global.verbose) {
    for (std::size_t index = 0; index < actions.size(); ++index) {
      ctx.ui->verbose("dependency scan: " + actions[index]->label + " (" +
                      std::to_string(results[index].milliseconds) + "ms)" +
                      (results[index].succeeded ? "" : " [failed]"));
    }
  }
}

// Runs the shared preparation pipeline. Returns nullopt when a diagnostic has
// already been printed and the caller should just fail.
std::optional<int> prepare(CommandContext& ctx, PreparedBuild& out, bool include_tests) {
  const auto prepare_started = Clock::now();
  auto project = load_project(ctx);
  if (!project) {
    return report_error(project.error(), ctx);
  }
  out.project = std::move(*project);
  for (const auto& warning : out.project.warnings) {
    ctx.ui->warn(warning);
  }

  auto target = resolve_target_from(ctx, out.project);
  if (!target) {
    return report_error(target.error(), ctx);
  }
  out.target = *target;
  if (ctx.global.verbose) {
    ctx.ui->verbose("project load: " + elapsed_ms(prepare_started) + "ms");
  }

  // Preparation can spend seconds probing compilers. Animate the stage on a
  // terminal; in a pipe each stage still prints one line when it starts.
  StageProgress stages(*ctx.ui, status_enabled(ctx), ctx.global.verbose,
                       ctx.global.use_color(stderr_is_tty()));

  // Resolve local dependencies and enforce the shared lockfile before any
  // expensive toolchain probing.
  const auto dependencies_started = Clock::now();
  stages.begin("Resolving dependencies");
  auto dependencies =
      resolve_dependencies(ctx, out.project, out.target, /*apply_lock=*/true, include_tests);
  if (!dependencies) {
    stages.cancel();
    return report_error(dependencies.error(), ctx);
  }
  stages.finish();
  if (ctx.global.verbose) {
    ctx.ui->verbose("dependency resolution: " + elapsed_ms(dependencies_started) + "ms");
  }
  if (dependencies->lock_written) {
    ctx.ui->verbose("wrote " + fs::generic_string(dependencies->lockfile));
  } else if (dependencies->lock_up_to_date) {
    ctx.ui->verbose(fs::generic_string(dependencies->lockfile) + " is up to date");
  }

  auto config = effective_config(ctx);
  if (!config) {
    return report_error(config.error(), ctx);
  }

  auto request = make_resolution_request(ctx, out.project, out.target, *config);

  // In locked mode, pin the resolution to the lockfile identity.
  if (config->locked || ctx.global.locked_mode()) {
    auto lock_state = load_toolchain_lock(out.project, ctx);
    if (!lock_state) {
      return report_error(lock_state.error(), ctx);
    }
    out.lock_state = std::move(*lock_state);
    if (auto* pinned = out.lock_state.lock.for_target(out.target.to_string())) {
      request.pinned_id = pinned->toolchain_id;
      request.pinned_content_hash = pinned->content_hash;
      request.locked = true;
    } else {
      return report_error(
          Error::make("cova::lock::target_missing",
                      "`cova-toolchain.lock` has no entry for target " +
                          out.target.to_string())
              .with("run without --locked to resolve and lock this target"),
          ctx);
    }
  } else {
    auto lock_state = load_toolchain_lock(out.project, ctx);
    if (!lock_state) {
      return report_error(lock_state.error(), ctx);
    }
    out.lock_state = std::move(*lock_state);
  }

  // Resolution runs real compiler probes, which is the slowest and least
  // observable part of a cold build. Say so before waiting on it.
  const auto toolchain_started = Clock::now();
  stages.begin("Resolving toolchain for " + out.target.to_string());
  auto resolved = resolve_toolchain_or_report(ctx, request);
  if (!resolved) {
    stages.cancel();
    return 1;
  }
  stages.finish();
  out.toolchain = std::move(*resolved);
  if (ctx.global.verbose) {
    ctx.ui->verbose("toolchain resolution: " + elapsed_ms(toolchain_started) + "ms");
  }

  const auto planning_started = Clock::now();
  stages.begin("Planning build");
  auto plan = plan_with_dependencies(ctx, out.project, *dependencies, out.toolchain, out.target,
                                     include_tests);
  if (!plan) {
    stages.cancel();
    return report_error(plan.error(), ctx);
  }
  stages.finish();
  out.plan = std::move(*plan);
  if (ctx.global.verbose) {
    ctx.ui->verbose("planning: " + elapsed_ms(planning_started) + "ms");
  }

  auto compile_commands = write_compile_commands(out.project, out.plan);
  if (!compile_commands) {
    ctx.ui->warn("could not write compile_commands.json: " +
                 compile_commands.error().message);
  } else {
    ctx.ui->verbose("wrote " + fs::generic_string(*compile_commands));
  }

  for (const auto& note : out.plan.notes) {
    ctx.ui->verbose(note);
  }
  return std::nullopt;
}

// Writes the toolchain lockfile when it is stale and we are not in locked mode.
Status maybe_write_lock(CommandContext& ctx, const PreparedBuild& prepared) {
  if (ctx.global.locked_mode()) {
    return ok();
  }
  bool needs_write = prepared.lock_state.should_write || !prepared.lock_state.present;
  if (!needs_write) {
    auto entry = lockfile::lock_entry_from(prepared.toolchain);
    auto existing = prepared.lock_state.lock.for_target(prepared.target.to_string());
    needs_write = existing == nullptr ||
                  lockfile::needs_rewrite(prepared.lock_state.lock, prepared.toolchain,
                                          ctx.global.locked_mode());
  }
  if (!needs_write) {
    return ok();
  }
  auto status = store_toolchain_lock(prepared.project, prepared.lock_state, prepared.toolchain);
  if (status) {
    ctx.ui->verbose("wrote " + fs::generic_string(prepared.project.layout.toolchain_lock));
  }
  return status;
}

// Verifies that the artifacts Cova produced really are for the requested
// target (PRD §22.4). This catches a host compiler that silently ignored
// `--target`, which is exactly the class of bug cross-compilation must not hide.
Status verify_artifacts(const PreparedBuild& prepared, CommandContext& ctx) {
  for (const auto& output : prepared.plan.expected_outputs) {
    if (!fs::is_regular_file(output)) {
      return fail(Error::make("cova::build::artifact_missing",
                              "the build reported success but the artifact is missing")
                      .with("path: " + fs::generic_string(output)));
    }
    const std::string extension = str::to_lower(output.extension().string());
    if (extension == ".a" || extension == ".lib" || extension == ".o" ||
        extension == ".obj" || extension == ".d") {
      continue;
    }
    auto info = platform::inspect_binary(output);
    if (!info) {
      return fail(Error::make("cova::build::artifact_uninspectable",
                              "the build produced an artifact Cova cannot inspect")
                      .with("path: " + fs::generic_string(output))
                      .with(info.error().message));
    }
    if (info->format == platform::BinaryFormat::Unknown) {
      return fail(Error::make("cova::build::artifact_unknown_format",
                              "the build produced an artifact whose format is not recognised")
                      .with("path: " + fs::generic_string(output))
                      .with("Cova recognises ELF, PE/COFF, Mach-O and WebAssembly"));
    }
    if (info->format == platform::BinaryFormat::Archive) {
      continue;
    }
    if (!platform::binary_matches_target(*info, prepared.target)) {
      return fail(Error::make(
          "cova::build::artifact_arch_mismatch",
          "artifact " + fs::generic_string(output) + " does not match the requested target")
          .with("expected: " + prepared.target.to_string())
          .with("found: " + info->to_string())
          .with("the compiler produced an artifact for a different architecture"));
    }
    ctx.ui->verbose("verified " + fs::generic_string(output) + ": " + info->to_string());
  }
  return ok();
}

// Executes a planned graph with incremental state and the shared artifact cache.
// `own_json` is false when the caller emits its own JSON document (`cova test`),
// so the build stage must not print a second, unrelated one to stdout.
int execute_build(CommandContext& ctx, PreparedBuild& prepared, bool dry_run,
                  bool own_json = true) {
  auto config = effective_config(ctx);
  if (!config) {
    return report_error(config.error(), ctx);
  }

  // Persisted incremental state lives beside the target directory.
  const auto state_file = prepared.project.layout.target_dir /
                          target_dir_component(prepared.target) / ".cova" / "state.toml";

  auto state = build::BuildState::load(state_file);
  build::BuildState local_state;
  if (state) {
    local_state = std::move(*state);
  }

  auto environment = make_action_environment(prepared, ctx);
  if (!environment) {
    return report_error(environment.error(), ctx);
  }
  build::ActionEnvironment env = std::move(*environment);

  build::ExecuteOptions options;
  options.max_parallel = config->resolved_jobs();
  options.incremental = true;
  options.fail_fast = !ctx.flags.boolean("keep-going");
  options.verbose = ctx.global.verbose;
  options.quiet = ctx.global.quiet;
  options.use_color = ctx.global.build_diagnostic_color();

  build::CacheSettings cache = build::default_cache_settings();
  cache.enabled = cache.enabled && !ctx.global.offline_mode();

  const auto started = Clock::now();
  std::size_t restored = 0;
  // Actions the local incremental record proved current in this run. Their
  // existing records already hold the verified hashes, so rebuilding them after
  // execution would re-hash the whole closure for no gain.
  std::set<std::uint64_t> verified_by_state;

  auto try_cache_restore = [&](const build::Action& action) -> bool {
    // Sharing requires a hermetic environment and a complete input closure.
    if (!cache.enabled || !action.cacheable || !env.shared_cache_eligible ||
        !action.input_closure_complete || !action.input_identity_complete) {
      return false;
    }
    // The key must be recomputed when another action in this build could have
    // changed this action's inputs, because the executor computed every key
    // before anything ran (a link's identity covers object files an earlier
    // action may only just have produced). An action with no dependencies has
    // no such producer, so the executor's up-front key is still exact and can
    // be reused instead of hashing the whole closure a second time.
    const std::string key = action.dependencies.empty() && !action.computed_key.empty()
                                ? action.computed_key
                                : action.cache_key(env);
    if (!action.input_identity_complete) {
      return false;
    }
    auto restored_ok = build::cache_restore(cache, action, key);
    if (restored_ok && *restored_ok) {
      ++restored;
      return true;
    }
    return false;
  };
  auto is_up_to_date = [&](const build::Action& action, std::string& reason) -> bool {
    // 1. The shared cache can restore every output without running anything.
    //    A dry run must not write artifacts, so it consults the incremental
    //    record only and reports a cache-only hit as stale.
    if (!dry_run && try_cache_restore(action)) {
      reason = "restored from cache";
      return true;
    }
    // 2. The local incremental record says the outputs are current. An
    //    untracked environment or an incomplete input identity may not be
    //    reused across executions at all.
    if (!action.input_identity_complete) {
      reason = "the input identity is incomplete";
      return false;
    }
    if (env.incremental_eligible && local_state.is_up_to_date(action, reason)) {
      verified_by_state.insert(action.id.value);
      return true;
    }
    if (env.incremental_eligible && local_state.inputs_match(action)) {
      // The recorded closure is still exact; only the output is missing or
      // stale. Reuse the recorded closure instead of paying for a scan.
      reason = "inputs match the previous record; rebuilding the output";
      return false;
    }
    // 3. Both checks missed. Before running the action, ask the provider for
    //    the real input closure, so a stale depfile or a newly included header
    //    cannot be missed. The refreshed inputs produce the final identity.
    if (action.kind == build::ActionKind::Compile && !dry_run &&
        !action.input_closure_checked) {
      const auto scan_started = Clock::now();
      std::string detail;
      auto refreshed = refresh_compile_closure(action, env, detail);
      if (ctx.global.verbose) {
        ctx.ui->verbose("dependency scan: " + action.label + " (" +
                        elapsed_ms(scan_started) + "ms)");
      }
      if (!refreshed) {
        reason = "dependency discovery failed: " + refreshed.error().message;
        return false;
      }
      if (*refreshed) {
        if (!action.input_identity_complete) {
          reason = "the input identity is incomplete";
          return false;
        }
        if (try_cache_restore(action)) {
          reason = "restored from cache after dependency discovery";
          return true;
        }
        if (env.incremental_eligible && local_state.is_up_to_date(action, reason)) {
          verified_by_state.insert(action.id.value);
          return true;
        }
      }
    }
    if (!env.incremental_eligible) {
      reason = "environment is untracked";
    }
    return false;
  };

  if (dry_run) {
    build::Executor executor(prepared.plan.graph, env, options, is_up_to_date);
    auto result = executor.dry_run();
    if (!result) {
      return report_error(result.error(), ctx);
    }
    ctx.ui->info("dry run: " + std::to_string(prepared.plan.graph.size()) + " action(s) planned");
    for (const auto& action : prepared.plan.graph.actions_in_order()) {
      std::string reason;
      const bool fresh = is_up_to_date(*action, reason);
      ctx.ui->out(std::string(fresh ? "  up-to-date  " : "  stale       ") + action->label);
    }
    return 0;
  }

  // From here on the build can take minutes. Report each action as it happens;
  // on a terminal the reporter redraws one line (with a heartbeat), in a pipe
  // it prints one line per finished action. It also owns the "checking what
  // needs rebuilding" phase, whose spinner covers the incremental check before
  // the first action starts.
  const bool show_status = status_enabled(ctx);
  ProgressReporter::Options progress;
  progress.header = "Compiling `" + project_label(prepared.project) + "`";
  const std::string& version = prepared.project.manifest.package.version;
  if (!version.empty()) {
    progress.header += " v" + version;
  }
  progress.header += " (" + selected_profile(ctx) + ") with " +
                     prepared.toolchain.descriptor.label() + " for " +
                     prepared.target.to_string();
  progress.total = prepared.plan.graph.size();
  progress.verbose = ctx.global.verbose;
  progress.use_color = ctx.global.use_color(stderr_is_tty());
  ProgressReporter reporter(*ctx.ui, std::move(progress), show_status);

  options.on_start = [&reporter](const build::Action& action) {
    reporter.action_started(action);
  };
  options.on_finish = [&](const build::Action& action, const build::ActionOutcome& outcome) {
    reporter.action_finished(action, outcome);
    if (!ctx.global.quiet && outcome.skipped && ctx.global.verbose) {
      ctx.ui->verbose(action.label + " (" + outcome.skip_reason + ")");
    }
  };

  // A fresh build has no state and no cache entries, so every compile action
  // needs a scan. Run those scans in parallel before the executor serializes
  // its up-to-date checks. When state or cache evidence exists, the executor's
  // normal miss path handles the (usually few) scans.
  if (!dry_run && local_state.size() == 0 && !cache_has_entries(cache)) {
    run_cold_dependency_scans(prepared.plan.graph, env, options.max_parallel, ctx);
  }

  // The executor snapshots its options, so the callbacks must be installed
  // before it is constructed.
  const auto execution_started = Clock::now();
  build::Executor executor(prepared.plan.graph, env, options, is_up_to_date);
  auto result = executor.run();
  if (ctx.global.verbose) {
    ctx.ui->verbose("build execution: " + elapsed_ms(execution_started) + "ms");
  }
  // Clear the transient line before diagnostics or the summary are printed.
  reporter.stop();
  if (!result) {
    return report_error(result.error(), ctx);
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);

  // Build the final records first; nothing is published until the whole build
  // and its artifact validation succeed. Successful compiles additionally add
  // the headers discovered through their depfile, so both the incremental
  // record and the cache entry cover the real input closure.
  std::vector<std::pair<build::Action, std::string>> publications;
  for (const auto& outcome : result->outcomes) {
    if (!outcome.succeeded) {
      continue;
    }
    // The incremental predicate already matched this action's key and verified
    // every recorded input and output hash in this run, so the record it
    // matched is still exact. Rebuilding it would re-hash the whole closure to
    // reproduce a record we already have, which made a no-op build as expensive
    // as a full rebuild.
    if (outcome.skipped && verified_by_state.contains(outcome.id.value)) {
      continue;
    }
    const build::Action* action = prepared.plan.graph.find(outcome.id);
    if (action == nullptr) {
      continue;
    }
    build::Action key_action = *action;
    if (action->input_closure_scan_failed) {
      if (ctx.global.verbose) {
        ctx.ui->verbose(action->label +
                        ": dependency scan failed; not recording or publishing");
      }
      continue;
    }
    if (!action->input_identity_complete) {
      if (ctx.global.verbose) {
        ctx.ui->verbose(action->label + ": incomplete input identity; not publishing");
      }
      continue;
    }
    bool closure_verified = true;
    std::string stability_detail;
    if (!action->inputs_stable(&stability_detail)) {
      closure_verified = false;
      if (ctx.global.verbose) {
        ctx.ui->verbose(action->label + ": " + stability_detail);
      }
    }
    build::ActionRecord record;
    // A compile's declared closure and its parsed depfile overlap almost
    // entirely, and the old linear scan over `record.input_hashes` made this
    // quadratic in the number of headers.
    std::set<std::string> recorded_inputs;
    auto add_input = [&](const fs::Path& input) {
      const std::string path = fs::generic_string(input);
      if (!recorded_inputs.insert(path).second) {
        return;
      }
      record.input_paths.push_back(path);
      auto digest = build::FileHasher{}.hash_file(input);
      if (digest) {
        record.input_hashes.emplace_back(path, cova::to_hex(*digest));
      }
    };
    for (const auto& output : action->outputs) {
      const std::string path = fs::generic_string(output.path);
      record.output_paths.push_back(path);
      auto digest = build::FileHasher{}.hash_file(output.path);
      if (digest) {
        record.output_hashes.emplace_back(path, cova::to_hex(*digest));
      }
    }
    for (const auto& input : action->inputs) {
      add_input(input.path);
    }
    if (action->kind == build::ActionKind::Compile && !action->outputs.empty() &&
        !outcome.skipped) {
      fs::Path depfile = action->outputs.front().path;
      depfile += ".d";
      auto parsed = build::Depfile::load(depfile, prepared.project.layout.root);
      if (!parsed) {
        // Without a depfile we cannot prove the input closure; the action may
        // run again next time, but it must not be reused or published.
        continue;
      }
      std::set<std::string> key_inputs;
      for (const auto& input : key_action.inputs) {
        key_inputs.insert(fs::generic_string(fs::canonical_for_compare(input.path)));
      }
      for (const auto& header : parsed->dependencies) {
        if (!fs::is_regular_file(header)) {
          continue;
        }
        add_input(header);
        const bool already_an_input =
            key_inputs.contains(fs::generic_string(fs::canonical_for_compare(header)));
        if (!already_an_input) {
          // A header the pre-execution scan did not see means the closure is
          // not proven for this execution. The output may satisfy the user,
          // but it must not become a reusable cache or incremental record.
          closure_verified = false;
          key_action.inputs.push_back(
              build::FileInput{header, "header", build::InputOrigin::Observed});
        }
      }
      key_action.input_closure_complete = closure_verified;
    }
    if (!closure_verified) {
      if (ctx.global.verbose) {
        ctx.ui->verbose(action->label + ": input closure was not verified; not publishing");
      }
      continue;
    }
    record.cache_key = key_action.cache_key(env);
    if (!key_action.input_identity_complete) {
      if (ctx.global.verbose) {
        ctx.ui->verbose(action->label +
                        ": input identity became incomplete; not recording or publishing");
      }
      continue;
    }
    record.toolchain_fingerprint = env.toolchain_fingerprint;
    record.sysroot_fingerprint = env.sysroot_fingerprint;
    record.completed_at_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
    record.duration_ms = static_cast<std::uint32_t>(outcome.duration.count());
    record.succeeded = true;
    if (!outcome.skipped && env.shared_cache_eligible && action->cacheable &&
        key_action.input_closure_complete && key_action.input_identity_complete) {
      publications.emplace_back(std::move(key_action), record.cache_key);
    }
    local_state.record(std::move(record));
  }

  if (!result->succeeded) {
    report_diagnostics(result->diagnostics, ctx);
    if (ctx.global.json && own_json) {
      json::Value out = json::Value::object();
      out.set("success", false);
      out.set("failed", static_cast<std::int64_t>(result->failed));
      out.set("executed", static_cast<std::int64_t>(result->executed));
      ctx.ui->out(out.dump(2));
    }
    return 1;
  }

  // A successful command line is not proof of a correct artifact. Check the
  // produced binaries against the frozen target before reporting success.
  auto verified = verify_artifacts(prepared, ctx);
  if (!verified) {
    return report_error(verified.error(), ctx);
  }

  // Publish only after execution succeeded, every output exists, and the
  // artifact-level verification passed. A failed or partial build leaves no
  // cache entry and no incremental record that a later build could trust.
  for (auto& [action, key] : publications) {
    auto stored = build::cache_store(cache, action, key);
    if (!stored) {
      if (stored.error().code == "cova::build::cache_nondeterminism") {
        ctx.ui->warn("cache entry for `" + action.label +
                     "` was not replaced: the same identity produced different artifacts");
      } else if (ctx.global.verbose) {
        ctx.ui->verbose("could not publish cache entry for " + action.label + ": " +
                        stored.error().message);
      }
    }
  }
  if (local_state.size() > 0) {
    auto save = local_state.save(state_file);
    if (!save) {
      ctx.ui->verbose("could not persist build state: " + save.error().message);
    }
  }

  if (ctx.global.json && own_json) {
    json::Value out = json::Value::object();
    out.set("success", true);
    out.set("executed", static_cast<std::int64_t>(result->executed));
    out.set("skipped", static_cast<std::int64_t>(result->skipped));
    out.set("restored_from_cache", static_cast<std::int64_t>(restored));
    out.set("duration_ms", static_cast<std::int64_t>(elapsed.count()));
    auto artifacts = json::Value::array();
    for (const auto& output : prepared.plan.expected_outputs) {
      artifacts.push(fs::generic_string(output));
    }
    out.set("artifacts", std::move(artifacts));
    ctx.ui->out(out.dump(2));
    return 0;
  }

  // The human summary goes to stdout, so it must stay out of the way of a JSON
  // document -- whether this stage prints one or the caller does.
  if (!ctx.global.quiet && !ctx.global.json) {
    const std::string verb = result->executed == 0 ? "Finished" : "Built";
    std::string line = verb + " `" + project_label(prepared.project) + "` in " +
                       format_duration(elapsed);
    if (result->skipped > 0) {
      line += " (" + std::to_string(result->skipped) + " up to date)";
    }
    if (restored > 0) {
      line += " (" + std::to_string(restored) + " from cache)";
    }
    ctx.ui->info(line);
  }
  for (const auto& output : prepared.plan.expected_outputs) {
    ctx.ui->verbose("artifact: " + fs::generic_string(output));
  }

  auto lock_status = maybe_write_lock(ctx, prepared);
  if (!lock_status) {
    ctx.ui->warn("could not update cova-toolchain.lock: " + lock_status.error().message);
  }
  return 0;
}

// Chooses the program to run for `cova run` (§13.1).
Result<fs::Path> pick_runnable(const PreparedBuild& prepared, CommandContext& ctx) {
  if (auto explicit_bin = ctx.flags.get("bin"); explicit_bin && !explicit_bin->empty()) {
    for (const auto& candidate : prepared.plan.expected_outputs) {
      if (candidate.stem().string() == *explicit_bin) {
        return candidate;
      }
    }
    return fail<fs::Path>(Error::make("cova::run::no_such_binary",
                                      "no executable named `" + *explicit_bin + "` was built"));
  }
  if (prepared.plan.runnable_binary) {
    return *prepared.plan.runnable_binary;
  }
  return fail<fs::Path>(
      Error::make("cova::run::nothing_to_run",
                  "this package has no executable target to run")
          .with("mark it as executable with `<name> = { type = \"executable\" }` in [build]")
          .with("or add `cova new <name>` for a runnable template"));
}

// Wraps a target program in the configured runner (§13.3).
Result<std::pair<fs::Path, std::vector<std::string>>> apply_runner(
    const PreparedBuild& prepared, CommandContext& ctx, const fs::Path& binary,
    const std::vector<std::string>& args) {
  auto config = effective_config(ctx);
  std::string runner;
  if (auto explicit_runner = ctx.flags.get("runner"); explicit_runner && !explicit_runner->empty()) {
    runner = *explicit_runner;
  } else if (config) {
    runner = config->user.runtime.default_runner;
  }
  if (config && config->user.runtime.prefer_native && prepared.target.is_native_like()) {
    runner.clear();
  }

  if (runner.empty() || runner == "native") {
    if (!prepared.target.is_native_like()) {
      return fail<std::pair<fs::Path, std::vector<std::string>>>(
          Error::make("cova::run::not_executable_here",
                      "target " + prepared.target.to_string() +
                          " cannot run on this host without a runner")
              .with("configure one with `--runner qemu` or in ~/.cova/config.toml")
              .with("Cova will not attempt to run a cross-compiled binary directly"));
    }
    return std::make_pair(binary, args);
  }

  std::string command;
  if (config) {
    command = config->user.runtime.qemu_prefix;
  }
  if (runner == "qemu") {
    if (command.empty()) {
      command = "qemu-" + std::string(prepared.target.arch == platform::Arch::Aarch64
                                          ? "aarch64"
                                          : prepared.target.arch == platform::Arch::Riscv64
                                                ? "riscv64"
                                                : "x86_64");
    }
  } else if (runner == "wine") {
    command = "wine";
  } else if (runner == "remote") {
    return fail<std::pair<fs::Path, std::vector<std::string>>>(
        Error::make("cova::run::runner_unsupported",
                    "remote execution is not implemented in this milestone")
            .with("configure a concrete runner command in ~/.cova/config.toml"));
  } else {
    command = runner;
  }

  for (const auto& entry : fs::path_entries()) {
    if (proc::is_executable(entry / command)) {
      std::vector<std::string> wrapped;
      wrapped.push_back(fs::generic_string(binary));
      wrapped.insert(wrapped.end(), args.begin(), args.end());
      return std::make_pair(entry / command, wrapped);
    }
  }
  return fail<std::pair<fs::Path, std::vector<std::string>>>(
      Error::make("cova::run::runner_missing",
                  "runner executable `" + command + "` was not found on PATH")
          .with("install it, or choose another runner with --runner"));
}

}  // namespace

int cmd_build(CommandContext& ctx) {
  PreparedBuild prepared;
  if (auto early = prepare(ctx, prepared, /*include_tests=*/false)) {
    return *early;
  }
  return execute_build(ctx, prepared, ctx.flags.boolean("dry-run"));
}

int cmd_check(CommandContext& ctx) {
  PreparedBuild prepared;
  if (auto early = prepare(ctx, prepared, /*include_tests=*/false)) {
    return *early;
  }
  // `cova check` compiles translation units but stops before archive/link.
  auto config = effective_config(ctx);
  if (!config) {
    return report_error(config.error(), ctx);
  }
  auto environment = make_action_environment(prepared, ctx);
  if (!environment) {
    return report_error(environment.error(), ctx);
  }
  build::ActionEnvironment env = std::move(*environment);

  auto state = build::BuildState::load(prepared.project.layout.target_dir /
                                       target_dir_component(prepared.target) / ".cova" /
                                       "state.toml");
  build::BuildState local_state;
  if (state) {
    local_state = std::move(*state);
  }

  build::ExecuteOptions options;
  options.max_parallel = config->resolved_jobs();
  options.quiet = ctx.global.quiet;
  options.verbose = ctx.global.verbose;
  options.use_color = ctx.global.build_diagnostic_color();

  ProgressReporter::Options progress;
  progress.header = "Checking `" + project_label(prepared.project) + "` with " +
                    prepared.toolchain.descriptor.label() + " for " +
                    prepared.target.to_string();
  progress.total = prepared.plan.graph.size();
  progress.verbose = ctx.global.verbose;
  progress.use_color = ctx.global.use_color(stderr_is_tty());
  ProgressReporter reporter(*ctx.ui, std::move(progress), status_enabled(ctx));

  options.on_start = [&reporter](const build::Action& action) {
    reporter.action_started(action);
  };
  options.on_finish = [&](const build::Action& action, const build::ActionOutcome& outcome) {
    reporter.action_finished(action, outcome);
    if (!ctx.global.quiet && outcome.skipped && ctx.global.verbose) {
      ctx.ui->verbose(action.label + " (" + outcome.skip_reason + ")");
    }
  };

  build::Executor executor(
      prepared.plan.graph, env, options,
      [&](const build::Action& action, std::string& reason) {
        if (action.kind != build::ActionKind::Compile) {
          // Skip everything that is not a compile step.
          reason = "check does not link";
          return true;
        }
        if (!env.incremental_eligible) {
          reason = "environment is untracked";
          return false;
        }
        return local_state.is_up_to_date(action, reason);
      });
  auto result = executor.run();
  reporter.stop();
  if (!result) {
    return report_error(result.error(), ctx);
  }
  if (!result->succeeded) {
    return report_diagnostics(result->diagnostics, ctx);
  }
  if (!ctx.global.quiet) {
    ctx.ui->info("Checked `" + project_label(prepared.project) + "`");
  }
  return 0;
}

int cmd_run(CommandContext& ctx) {
  PreparedBuild prepared;
  if (auto early = prepare(ctx, prepared, /*include_tests=*/false)) {
    return *early;
  }
  // `cova run --dry-run` reports what would be built and then stops; building
  // first and only checking the flag afterwards made the dry run the real thing.
  const bool dry_run = ctx.flags.boolean("dry-run");
  const int build_code = execute_build(ctx, prepared, dry_run);
  if (build_code != 0) {
    return build_code;
  }
  if (dry_run) {
    return 0;
  }

  auto binary = pick_runnable(prepared, ctx);
  if (!binary) {
    return report_error(binary.error(), ctx);
  }
  auto invocation = apply_runner(prepared, ctx, *binary, ctx.passthrough());
  if (!invocation) {
    return report_error(invocation.error(), ctx);
  }

  // In an interactive terminal the program should own the terminal streams.
  // In tests and pipelines Cova captures and relays the output so callers can
  // assert on it; the process still receives our stdin.
  const bool interactive = ctx.ui->interactive();
  proc::ProcessSpec spec;
  spec.program = invocation->first;
  spec.args = invocation->second;
  spec.cwd = prepared.project.layout.root;
  spec.inherit_stdout = interactive;
  spec.inherit_stderr = interactive;
  spec.inherit_stdin = true;

  if (!ctx.global.quiet) {
    ctx.ui->verbose("running " + proc::format_command_line(spec));
  }
  auto result = proc::run(spec);
  if (!result) {
    return report_error(result.error(), ctx);
  }
  if (!interactive) {
    // With --json, stdout carries exactly one JSON document (the build report),
    // so the program's own output goes to stderr instead of corrupting it.
    if (!result->stdout_text.empty()) {
      if (ctx.global.json) {
        ctx.ui->raw_error(result->stdout_text);
      } else {
        ctx.ui->out(result->stdout_text);
      }
    }
    if (!result->stderr_text.empty()) {
      ctx.ui->error(result->stderr_text);
    }
  }
  if (result->timed_out) {
    return report_error(Error::make("cova::run::timeout", "the program timed out"), ctx);
  }
  if (!result->success()) {
    return result->exit_code == 0 ? 1 : result->exit_code;
  }
  return 0;
}

int cmd_test(CommandContext& ctx) {
  PreparedBuild prepared;
  if (auto early = prepare(ctx, prepared, /*include_tests=*/true)) {
    return *early;
  }
  if (prepared.plan.test_binaries.empty() && prepared.plan.graph.empty()) {
    ctx.ui->info("no tests declared for `" + project_label(prepared.project) + "`");
    return 0;
  }
  const int build_code = execute_build(ctx, prepared, /*dry_run=*/false, /*own_json=*/false);
  if (build_code != 0) {
    return build_code;
  }

  if (prepared.plan.test_binaries.empty()) {
    ctx.ui->info("no tests to run");
    return 0;
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;

  // Run test programs sequentially: parallel test output interleaves badly and
  // tests often contend for the same ports and temp directories.
  auto report_failure = [&](const std::string& label, const std::string& reason) {
    ++failed;
    failures.push_back(label + ": " + reason);
    ctx.ui->error("test failed: " + label);
    // Under --json the failure detail belongs to the JSON document (it is a
    // field of `failures`) and to stderr, never to stdout: writing it here made
    // the machine-readable output unparseable by prefixing it to the JSON.
    if (!reason.empty()) {
      if (ctx.global.json) {
        ctx.ui->error(reason);
      } else {
        ctx.ui->out(reason);
      }
    }
  };

  for (const auto& binary : prepared.plan.test_binaries) {
    std::vector<std::string> args = ctx.passthrough();
    if (auto filter = ctx.flags.get("filter"); filter && !filter->empty()) {
      args.push_back(*filter);
    }
    auto invocation = apply_runner(prepared, ctx, binary, args);
    if (!invocation) {
      return report_error(invocation.error(), ctx);
    }
    proc::ProcessSpec spec;
    spec.program = invocation->first;
    spec.args = invocation->second;
    spec.cwd = prepared.project.layout.root;
    spec.inherit_stdout = !ctx.global.json;
    spec.inherit_stderr = !ctx.global.json;

    if (!ctx.global.quiet && !ctx.global.json) {
      ctx.ui->info("running " + binary.filename().string());
    }
    auto result = proc::run(spec);
    if (!result) {
      return report_error(result.error(), ctx);
    }
    const std::string label = binary.filename().string();
    if (result->timed_out) {
      report_failure(label, "timed out");
    } else if (result->success()) {
      ++passed;
    } else {
      report_failure(label, "exit code " + std::to_string(result->exit_code) + "\n" +
                                result->stderr_text);
    }
  }

  if (ctx.global.json) {
    json::Value out = json::Value::object();
    out.set("success", failed == 0);
    out.set("passed", static_cast<std::int64_t>(passed));
    out.set("failed", static_cast<std::int64_t>(failed));
    auto failure_array = json::Value::array();
    for (const auto& failure : failures) {
      failure_array.push(failure);
    }
    out.set("failures", std::move(failure_array));
    ctx.ui->out(out.dump(2));
    return failed == 0 ? 0 : 1;
  }

  if (!ctx.global.quiet) {
    if (failed == 0) {
      ctx.ui->info("test result: ok. " + std::to_string(passed) + " passed");
    } else {
      ctx.ui->error("test result: FAILED. " + std::to_string(passed) + " passed; " +
                    std::to_string(failed) + " failed");
    }
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace cova::cli
