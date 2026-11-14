// Cova — parallel action executor (PRD §9.4, §19.2).
#include "build/executor/executor.hpp"

#include <optional>

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "build/scheduler/scheduler.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/process/process.hpp"

namespace cova::build {
namespace {

std::string failure_detail(const proc::ProcessResult& result) {
  std::string detail = str::trim(result.stderr_text);
  if (detail.empty()) {
    detail = str::trim(result.stdout_text);
  }
  if (detail.size() > 2000) {
    detail = detail.substr(detail.size() - 2000);
  }
  return detail;
}

int parallelism(int requested) {
  if (requested > 0) {
    return requested;
  }
  const unsigned hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : static_cast<int>(hardware);
}

}  // namespace

Executor::Executor(const BuildGraph& graph, ActionEnvironment environment,
                   ExecuteOptions options, UpToDateCheck up_to_date_check)
    : graph_(graph),
      environment_(std::move(environment)),
      options_(std::move(options)),
      up_to_date_check_(std::move(up_to_date_check)) {}

ActionOutcome Executor::run_action(const Action& action) {
  ActionOutcome outcome;
  outcome.id = action.id;
  const auto started = std::chrono::steady_clock::now();

  if (action.argv.empty()) {
    for (const auto& output : action.outputs) {
      if (!fs::is_regular_file(output.path)) {
        outcome.failure_message = "the action declares output `" +
                                  fs::generic_string(output.path) + "` but has no command";
        return outcome;
      }
    }
    outcome.succeeded = true;
    outcome.artifact_validated = true;
    outcome.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return outcome;
  }
  for (const auto& output : action.outputs) {
    if (output.path.has_parent_path()) {
      auto created = fs::create_directories(output.path.parent_path());
      if (!created) {
        outcome.failure_message = created.error().message;
        return outcome;
      }
    }
  }

  if (options_.on_start) {
    options_.on_start(action);
  }

  proc::ProcessSpec spec;
  spec.program = action.argv.front();
  spec.args.assign(action.argv.begin() + 1, action.argv.end());
  if (!action.working_dir.empty()) {
    spec.cwd = action.working_dir;
  }
  if (environment_.explicit_environment) {
    std::vector<std::pair<std::string, std::string>> variables;
    variables.reserve(environment_.environment.size());
    for (const auto& [name, value] : environment_.environment) {
      variables.emplace_back(name, value);
    }
    spec.environment = std::move(variables);
  }
  auto result = proc::run(spec);
  outcome.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  if (!result) {
    outcome.failure_message = result.error().message;
    return outcome;
  }
  outcome.exit_code = result->exit_code;
  if (!result->success()) {
    outcome.failure_message = failure_detail(*result);
    return outcome;
  }
  // ExecutionSucceeded is not ArtifactValidated: a compiler may exit 0 while a
  // declared output is missing. Only a complete output set may be published.
  for (const auto& output : action.outputs) {
    if (!fs::is_regular_file(output.path)) {
      outcome.failure_message =
          "the action reported success but did not produce `" + fs::generic_string(output.path) + "`";
      return outcome;
    }
  }
  outcome.succeeded = true;
  outcome.artifact_validated = true;
  // Sharing requires a hermetic environment, a complete input closure and an
  // action the planner marks shareable. The caller publishes under the final
  // (post-depfile) identity.
  outcome.share_eligible = action.cacheable && environment_.shared_cache_eligible &&
                           action.input_closure_complete && action.input_identity_complete;
  return outcome;
}

void Executor::note_outcome(const Action& action, const ActionOutcome& outcome,
                            BuildResult& result) {
  result.outcomes.push_back(outcome);
  if (outcome.skipped) {
    result.skipped += 1;
  } else if (outcome.succeeded) {
    result.executed += 1;
  } else {
    result.failed += 1;
    result.succeeded = false;
    Diagnostic diagnostic = make_error("cova::build::action_failed",
                                       "action failed: " + action.label);
    if (!outcome.failure_message.empty()) {
      diagnostic.line(outcome.failure_message);
    }
    result.diagnostics.emit(std::move(diagnostic));
  }
  if (options_.on_finish) {
    options_.on_finish(action, outcome);
  }
}

Result<BuildResult> Executor::dry_run() {
  BuildResult result;
  auto ordered = graph_.actions_in_order();
  for (const auto* action : ordered) {
    action->cache_key(environment_);
    ActionOutcome outcome;
    outcome.id = action->id;
    std::string reason;
    if (options_.incremental && up_to_date_check_ && up_to_date_check_(*action, reason)) {
      outcome.skipped = true;
      outcome.succeeded = true;
      outcome.skip_reason = reason;
    } else {
      outcome.succeeded = false;
      outcome.skip_reason = reason;
    }
    result.outcomes.push_back(std::move(outcome));
  }
  result.succeeded = true;
  return result;
}

Result<BuildResult> Executor::run() {
  BuildResult result;
  auto ordered = graph_.actions_in_order();
  // Compute cache keys once; the incremental predicate and the recorder both use
  // the same value.
  for (const auto* action : ordered) {
    action->cache_key(environment_);
  }

  // Dispatch straight from the dependency scheduler instead of walking the
  // graph in "depth levels". The level approach had a barrier: every action in
  // a level had to finish before any action of the next level started, so one
  // slow compile blocked ready work behind it. The scheduler releases each
  // dependent as soon as *its* dependencies are done.
  Scheduler scheduler(graph_);
  std::mutex scheduler_mutex;
  std::mutex check_mutex;
  bool stop_early = false;
  const int workers = parallelism(options_.max_parallel);

  enum class Disposition { Skip, Fail, Run };
  struct Running {
    const Action* action = nullptr;
    std::future<ActionOutcome> future;
  };

  auto settled = [&](const Action& action, const ActionOutcome& outcome) {
    {
      std::lock_guard<std::mutex> lock(scheduler_mutex);
      if (outcome.succeeded) {
        scheduler.complete(action.id);
      } else {
        scheduler.fail(action.id);
      }
    }
    note_outcome(action, outcome, result);
    if (!outcome.succeeded && options_.fail_fast) {
      stop_early = true;
    }
  };

  auto consider = [&](const Action& action, std::string& reason) -> Disposition {
    if (stop_early) {
      return Disposition::Fail;
    }
    bool current = false;
    if (options_.incremental && up_to_date_check_) {
      // The predicate consults shared caches and records state, so it stays
      // serialised. It runs on the executor thread only.
      std::lock_guard<std::mutex> lock(check_mutex);
      current = up_to_date_check_(action, reason);
    }
    if (current) {
      return Disposition::Skip;
    }
    return Disposition::Run;
  };

  std::vector<Running> in_flight;
  for (;;) {
    if (stop_early) {
      break;
    }
    // Launch as many ready actions as the worker budget allows.
    while (!stop_early &&
           static_cast<int>(in_flight.size()) < workers &&
           scheduler.ready_count() > 0) {
      std::optional<ActionId> next;
      {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        next = scheduler.next();
      }
      if (!next) {
        break;
      }
      const Action* action = graph_.find(*next);
      if (action == nullptr) {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        scheduler.complete(*next);
        continue;
      }
      std::string reason;
      const Disposition disposition = consider(*action, reason);
      if (disposition == Disposition::Skip) {
        ActionOutcome outcome;
        outcome.id = action->id;
        outcome.succeeded = true;
        outcome.skipped = true;
        // The predicate's reason distinguishes "the incremental record already
        // proved this current" from "restored from cache". The caller uses that
        // difference to keep an existing, still-exact record instead of
        // re-deriving one it already has.
        outcome.skip_reason = reason.empty() ? "up to date" : reason;
        settled(*action, outcome);
        continue;
      }
      if (disposition == Disposition::Fail) {
        ActionOutcome outcome;
        outcome.id = action->id;
        outcome.failure_message = "a dependency failed";
        settled(*action, outcome);
        continue;
      }
      in_flight.push_back(Running{action, std::async(std::launch::async, [this, action] {
                                    return run_action(*action);
                                  })});
    }

    if (in_flight.empty()) {
      break;
    }

    // Wait for the first action that finishes. Polling keeps the executor
    // single-threaded while still allowing completions in any order; the small
    // sleep is invisible next to a compile.
    bool progressed = false;
    for (std::size_t index = 0; index < in_flight.size();) {
      auto& entry = in_flight[index];
      if (entry.future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        ++index;
        continue;
      }
      ActionOutcome outcome = entry.future.get();
      settled(*entry.action, outcome);
      in_flight.erase(in_flight.begin() + static_cast<std::ptrdiff_t>(index));
      progressed = true;
      break;
    }
    if (!progressed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Drain anything still running so no future is abandoned mid-flight, and so
  // fail-fast does not hide a diagnostic the user needs from a started action.
  for (auto& entry : in_flight) {
    ActionOutcome outcome = entry.future.get();
    note_outcome(*entry.action, outcome, result);
  }

  return result;
}

}  // namespace cova::build
