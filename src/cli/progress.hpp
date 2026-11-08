// Cova — live build progress.
//
// The executor is a black box between "start" and "finish": a build that runs
// for a minute without printing anything is indistinguishable from one that has
// hung. Two reporters turn waiting into visible work:
//
//   * ProgressReporter follows build actions: on a terminal it animates one
//     line built around a progress bar (`━━━━━━────`) carrying the task count,
//     elapsed time and a status label such as `compiling src/foo.cpp`, and it
//     keeps ticking while a single compile runs; in a pipe, log or CI capture
//     it prints one line per finished action.
//   * StageProgress covers preparation (dependency resolution, toolchain
//     probing, planning), where a spinner with elapsed time is the honest
//     indication of work because the amount of work is unknown.
//
// Both stay quiet for `--quiet` and `--json`, and both fall back to plain lines
// for `--verbose` so debug messages never land in the middle of an animated
// line. `--json` keeps stdout purely machine-readable: progress goes to stderr.
//
// Thread ownership: `action_started` and the animation tickers run on worker
// threads; `action_finished`, stage transitions and `stop` run on the command
// thread. All rendering is serialised internally, so nothing interleaves
// mid-line.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "build/executor/executor.hpp"
#include "cli/cli.hpp"

namespace cova::cli {

class ProgressReporter {
 public:
  struct Options {
    // Printed once, lazily, before the first action that actually runs. Kept
    // lazy so a no-op build does not announce compilation it never performs.
    std::string header;
    std::size_t total = 0;
    // Verbose mode keeps the output stable (lines instead of a redrawn line) so
    // debug messages never land in the middle of a transient progress line.
    bool verbose = false;
    // Colour the spinner and progress bar. Only meaningful when `live`.
    bool use_color = false;
    // Shown while the executor hashes inputs and consults the cache, before the
    // first action starts.
    std::string check_label = "Checking what needs rebuilding";
  };

  ProgressReporter(Ui& ui, Options options, bool enabled);
  ~ProgressReporter();

  ProgressReporter(const ProgressReporter&) = delete;
  ProgressReporter& operator=(const ProgressReporter&) = delete;

  // Worker-thread callback: an action's command is about to run.
  void action_started(const build::Action& action);
  // Command-thread callback: an action finished, failed or was skipped.
  void action_finished(const build::Action& action, const build::ActionOutcome& outcome);
  // Stops the heartbeat and clears the transient line. Idempotent.
  void stop();

 private:
  struct RunningAction {
    std::uint64_t id = 0;
    build::ActionKind kind = build::ActionKind::Custom;
    std::string label;
  };

  void ensure_header_locked();
  void render_locked();
  std::string line_locked() const;

  Ui& ui_;
  Options options_;
  bool enabled_ = false;
  bool live_ = false;

  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::jthread ticker_;
  std::chrono::steady_clock::time_point started_;
  std::size_t completed_ = 0;
  // Planned actions minus those already known to be up to date or restored, so
  // the bar measures work that still has to run rather than work already
  // skipped. It only shrinks, so the bar never moves backwards.
  std::size_t remaining_ = 0;
  // Actions currently executing, in start order; the newest is the one shown.
  std::vector<RunningAction> running_;
  std::string last_label_;
  build::ActionKind last_kind_ = build::ActionKind::Custom;
  std::size_t frame_ = 0;
  // True until the first action actually starts: the reporter then animates the
  // incremental check instead of an empty bar.
  bool checking_ = false;
  bool header_printed_ = false;
  bool stopped_ = false;
};

// Animates one preparation stage. Because the remaining work in a stage is not
// known in advance, this is an indeterminate spinner rather than a bar.
//
// Terminal: the stage animates in place and, when it succeeds, becomes a
// permanent line with its duration. Pipe: the label is printed immediately and
// nothing is animated. Verbose/quiet/json: plain lines or nothing.
class StageProgress {
 public:
  StageProgress(Ui& ui, bool enabled, bool verbose, bool use_color);
  ~StageProgress();

  StageProgress(const StageProgress&) = delete;
  StageProgress& operator=(const StageProgress&) = delete;

  // Starts (or restarts) the animation for `label`.
  void begin(std::string label);
  // Ends the stage successfully: on a terminal the animated line is replaced by
  // a permanent `label  (duration)` line.
  void finish();
  // Ends the stage without a permanent line (failure path or scope exit).
  void cancel();

 private:
  void render();
  void stop_ticker();

  Ui& ui_;
  bool enabled_ = false;
  bool use_color_ = false;
  bool live_ = false;
  std::string label_;
  std::chrono::steady_clock::time_point started_{};
  std::size_t frame_ = 0;
  bool active_ = false;
  bool stop_requested_ = false;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::jthread ticker_;
};

}  // namespace cova::cli


