// Cova — `cova build`, `cova check`, `cova run`, `cova test` (PRD §9, §13).
//
// These four commands share one pipeline:
//   load project → resolve target → resolve toolchain (frozen) → plan graph
//   → restore fresh outputs from cache → execute → report
// Keeping it in one file makes the shared preconditions impossible to get wrong
// in one command but right in another.
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "build/cache/cache.hpp"
#include "build/executor/executor.hpp"
#include "build/planner.hpp"
#include "build/scheduler/scheduler.hpp"
#include "cli/common.hpp"
#include "core/diagnostics/diagnostic.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/json/json.hpp"
#include "core/process/process.hpp"

namespace cova::cli {
namespace {

using Clock = std::chrono::steady_clock;

// A fully prepared build: everything resolved and planned, nothing executed.
struct PreparedBuild {
  LoadedProject project;
  platform::Triple target;
  toolchain::ResolvedToolchain toolchain;
  build::PlanResult plan;
  LockState lock_state;
};

// Runs the shared preparation pipeline. Returns nullopt when a diagnostic has
// already been printed and the caller should just fail.
std::optional<int> prepare(CommandContext& ctx, PreparedBuild& out, bool include_tests) {
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

  auto resolved = resolve_toolchain_or_report(ctx, request);
  if (!resolved) {
    return 1;
  }
  out.toolchain = std::move(*resolved);

  auto plan = plan_for(ctx, out.project, out.toolchain, out.target, include_tests);
  if (!plan) {
    return report_error(plan.error(), ctx);
  }
  out.plan = std::move(*plan);

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

// Executes a planned graph with incremental state and the shared artifact cache.
int execute_build(CommandContext& ctx, PreparedBuild& prepared, bool dry_run) {
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

  build::ActionEnvironment env;
  env.target = prepared.target;
  env.abi = prepared.toolchain.abi_signature;
  env.toolchain_fingerprint = prepared.toolchain.fingerprint;
  env.sysroot_fingerprint = prepared.toolchain.sysroot_id;

  build::ExecuteOptions options;
  options.max_parallel = config->resolved_jobs();
  options.incremental = true;
  options.fail_fast = !ctx.flags.boolean("keep-going");
  options.verbose = ctx.global.verbose;
  options.quiet = ctx.global.quiet;
  options.use_color = ctx.global.use_color(stdout_is_tty());

  build::CacheSettings cache = build::default_cache_settings();
  cache.enabled = cache.enabled && !ctx.global.offline_mode();

  const auto started = Clock::now();
  std::size_t restored = 0;

  auto is_up_to_date = [&](const build::Action& action, std::string& reason) -> bool {
    // 1. The shared cache can restore every output without running anything.
    if (cache.enabled && action.cacheable) {
      const std::string key = action.cache_key(env);
      auto restored_ok = build::cache_restore(cache, action, key);
      if (restored_ok && *restored_ok) {
        ++restored;
        reason = "restored from cache";
        return true;
      }
    }
    // 2. The local incremental record says the outputs are current.
    return local_state.is_up_to_date(action, reason);
  };

  build::Executor executor(prepared.plan.graph, env, options, is_up_to_date);

  if (dry_run) {
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

  bool printed_progress = false;
  if (!ctx.global.quiet) {
    options.on_finish = [&](const build::Action& action, const build::ActionOutcome& outcome) {
      if (outcome.skipped) {
        if (ctx.global.verbose) {
          ctx.ui->verbose(action.label + " (" + outcome.skip_reason + ")");
        }
        return;
      }
      printed_progress = true;
      if (outcome.succeeded) {
        ctx.ui->verbose(action.label + "  " + format_duration(outcome.duration));
      }
    };
  }

  auto result = executor.run();
  if (!result) {
    return report_error(result.error(), ctx);
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
  (void)printed_progress;

  // Persist incremental state for successful actions.
  for (const auto& outcome : result->outcomes) {
    if (!outcome.succeeded || outcome.skipped) {
      continue;
    }
    const build::Action* action = prepared.plan.graph.find(outcome.id);
    if (action == nullptr) {
      continue;
    }
    build::ActionRecord record;
    record.cache_key = action->cache_key(env);
    for (const auto& output : action->outputs) {
      record.output_paths.push_back(fs::generic_string(output.path));
    }
    for (const auto& input : action->inputs) {
      record.input_paths.push_back(fs::generic_string(input.path));
      auto digest = build::FileHasher{}.hash_file(input.path);
      if (digest) {
        record.input_hashes.emplace_back(fs::generic_string(input.path), *digest);
      }
    }
    record.toolchain_fingerprint = env.toolchain_fingerprint;
    record.sysroot_fingerprint = env.sysroot_fingerprint;
    record.completed_at_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
    record.duration_ms = static_cast<std::uint32_t>(outcome.duration.count());
    record.succeeded = true;
    local_state.record(std::move(record));
  }
  if (local_state.size() > 0) {
    auto save = local_state.save(state_file);
    if (!save) {
      ctx.ui->verbose("could not persist build state: " + save.error().message);
    }
  }

  if (!result->succeeded) {
    report_diagnostics(result->diagnostics, ctx);
    if (ctx.global.json) {
      json::Value out = json::Value::object();
      out.set("success", false);
      out.set("failed", static_cast<std::int64_t>(result->failed));
      out.set("executed", static_cast<std::int64_t>(result->executed));
      ctx.ui->out(out.dump(2));
    }
    return 1;
  }

  if (ctx.global.json) {
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

  if (!ctx.global.quiet) {
    const std::string verb = result->executed == 0 ? "Finished" : "Built";
    std::string line = verb + " `" + prepared.project.manifest.package.name + "` in " +
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
  build::ActionEnvironment env;
  env.target = prepared.target;
  env.abi = prepared.toolchain.abi_signature;
  env.toolchain_fingerprint = prepared.toolchain.fingerprint;
  env.sysroot_fingerprint = prepared.toolchain.sysroot_id;

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
  options.use_color = ctx.global.use_color(stdout_is_tty());
  options.on_finish = [&](const build::Action& action, const build::ActionOutcome& outcome) {
    if (outcome.succeeded && !outcome.skipped && ctx.global.verbose) {
      ctx.ui->verbose(action.label);
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
        return local_state.is_up_to_date(action, reason);
      });
  auto result = executor.run();
  if (!result) {
    return report_error(result.error(), ctx);
  }
  if (!result->succeeded) {
    return report_diagnostics(result->diagnostics, ctx);
  }
  if (!ctx.global.quiet) {
    ctx.ui->info("Checked `" + prepared.project.manifest.package.name + "`");
  }
  return 0;
}

int cmd_run(CommandContext& ctx) {
  PreparedBuild prepared;
  if (auto early = prepare(ctx, prepared, /*include_tests=*/false)) {
    return *early;
  }
  const int build_code = execute_build(ctx, prepared, /*dry_run=*/false);
  if (build_code != 0) {
    return build_code;
  }
  if (ctx.flags.boolean("dry-run")) {
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

  proc::ProcessSpec spec;
  spec.program = invocation->first;
  spec.args = invocation->second;
  spec.cwd = prepared.project.layout.root;
  spec.inherit_stdout = true;
  spec.inherit_stderr = true;
  spec.inherit_stdin = true;

  if (!ctx.global.quiet) {
    ctx.ui->verbose("running " + proc::format_command_line(spec));
  }
  auto result = proc::run(spec);
  if (!result) {
    return report_error(result.error(), ctx);
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
    ctx.ui->info("no tests declared for `" + prepared.project.manifest.package.name + "`");
    return 0;
  }
  const int build_code = execute_build(ctx, prepared, /*dry_run=*/false);
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
    if (!reason.empty()) {
      ctx.ui->out(reason);
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
