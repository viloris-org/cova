// Cova — parallel action executor (PRD §9.4, §19.2).
#include "build/executor/executor.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "build/cache/cache.hpp"
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
    outcome.succeeded = true;
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

  proc::ProcessSpec spec;
  spec.program = action.argv.front();
  spec.args.assign(action.argv.begin() + 1, action.argv.end());
  if (!action.working_dir.empty()) {
    spec.cwd = action.working_dir;
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
  outcome.succeeded = true;
  if (action.cacheable && !action.computed_key.empty()) {
    auto stored = cache_store(default_cache_settings(), action, action.computed_key);
    (void)stored;
  }
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

  std::map<std::uint64_t, int> depth;
  for (const auto* action : ordered) {
    int level = 0;
    for (const auto& dependency : action->dependencies) {
      auto it = depth.find(dependency.value);
      if (it != depth.end()) {
        level = std::max(level, it->second + 1);
      }
    }
    depth[action->id.value] = level;
  }
  std::map<int, std::vector<const Action*>> levels;
  for (const auto* action : ordered) {
    levels[depth[action->id.value]].push_back(action);
  }

  std::map<std::uint64_t, bool> completed;
  bool stop_early = false;
  std::mutex check_mutex;
  const int workers = parallelism(options_.max_parallel);

  for (const auto& [level, actions] : levels) {
    (void)level;
    if (stop_early) {
      break;
    }
    std::vector<const Action*> runnable;
    for (const auto* action : actions) {
      bool dependency_failed = false;
      for (const auto& dependency : action->dependencies) {
        auto it = completed.find(dependency.value);
        if (it == completed.end() || !it->second) {
          dependency_failed = true;
          break;
        }
      }
      if (dependency_failed) {
        ActionOutcome outcome;
        outcome.id = action->id;
        outcome.failure_message = "a dependency failed";
        note_outcome(*action, outcome, result);
        completed[action->id.value] = false;
        stop_early = stop_early || options_.fail_fast;
        continue;
      }
      std::string reason;
      bool current = false;
      if (options_.incremental && up_to_date_check_) {
        std::lock_guard<std::mutex> lock(check_mutex);
        current = up_to_date_check_(*action, reason);
      }
      if (current) {
        ActionOutcome outcome;
        outcome.id = action->id;
        outcome.succeeded = true;
        outcome.skipped = true;
        outcome.skip_reason = reason;
        note_outcome(*action, outcome, result);
        completed[action->id.value] = true;
        continue;
      }
      runnable.push_back(action);
    }

    for (std::size_t offset = 0; offset < runnable.size() && !stop_early;
         offset += static_cast<std::size_t>(workers)) {
      std::vector<std::future<ActionOutcome>> futures;
      const std::size_t end =
          std::min(runnable.size(), offset + static_cast<std::size_t>(workers));
      for (std::size_t index = offset; index < end; ++index) {
        const Action* action = runnable[index];
        futures.push_back(std::async(std::launch::async, [this, action] {
          return run_action(*action);
        }));
      }
      for (std::size_t index = 0; index < futures.size(); ++index) {
        ActionOutcome outcome = futures[index].get();
        const Action* action = runnable[offset + index];
        note_outcome(*action, outcome, result);
        completed[action->id.value] = outcome.succeeded;
        if (!outcome.succeeded && options_.fail_fast) {
          stop_early = true;
        }
      }
    }
  }

  if (!result.succeeded) {
    return result;
  }
  return result;
}

}  // namespace cova::build
