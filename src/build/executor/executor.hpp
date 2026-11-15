// Cova — parallel action executor (PRD §9.4, §19.2).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "build/action/action.hpp"
#include "build/graph/graph.hpp"
#include "core/common.hpp"
#include "core/diagnostics/diagnostic.hpp"

namespace cova::build {

struct ActionOutcome {
  ActionId id;
  bool succeeded = false;
  bool skipped = false;
  std::string skip_reason;
  std::string failure_message;
  int exit_code = 0;
  std::chrono::milliseconds duration{0};
  // Wall-clock start of the action, in nanoseconds since the Unix epoch. Inputs
  // whose modification time is later than this may have been rewritten while
  // the action was running, so the artifact must not be reused.
  std::int64_t start_ns = 0;
  // ExecutionSucceeded, then each output exists (ArtifactValidated), then the
  // environment and input closure permit sharing (ShareEligible). Committed is
  // set by the caller that publishes the artifact cache entry.
  bool artifact_validated = false;
  bool share_eligible = false;
  bool committed = false;
};

struct BuildResult {
  bool succeeded = true;
  std::size_t executed = 0;
  std::size_t skipped = 0;
  std::size_t failed = 0;
  std::vector<ActionOutcome> outcomes;
  DiagnosticEngine diagnostics;
};

struct ExecuteOptions {
  int max_parallel = 0;  // 0 = hardware concurrency
  bool incremental = true;
  bool fail_fast = true;
  bool verbose = false;
  bool quiet = false;
  bool use_color = false;
  // Called from a worker thread immediately before an action's command starts.
  // Skipped actions never call it, so implementations must be thread-safe.
  std::function<void(const Action&)> on_start;
  // Called on the executor thread for every action after it is considered
  // (skipped or run).
  std::function<void(const Action&, const ActionOutcome&)> on_finish;
};

class Executor {
 public:
  // Returns true when the action is current and must not run. `reason` is
  // rendered in verbose mode.
  using UpToDateCheck = std::function<bool(const Action&, std::string&)>;

  Executor(const BuildGraph& graph, ActionEnvironment environment,
           ExecuteOptions options, UpToDateCheck up_to_date_check,
           FileIdentityCache* identity = nullptr);

  // Executes ready actions, respecting dependencies and the parallelism limit.
  Result<BuildResult> run();

  // Plans and reports without executing anything. Populates the plan identity
  // on every action so the caller can ask the incremental state the same
  // question.
  Result<BuildResult> dry_run();

 private:
  ActionOutcome run_action(const Action& action);
  void note_outcome(const Action& action, const ActionOutcome& outcome, BuildResult& result);

  const BuildGraph& graph_;
  ActionEnvironment environment_;
  ExecuteOptions options_;
  UpToDateCheck up_to_date_check_;
  FileIdentityCache* identity_ = nullptr;
};

}  // namespace cova::build
