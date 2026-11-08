// Cova — live build progress reporting implementation.
#include "cli/progress.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "cli/common.hpp"

namespace cova::cli {
namespace {

// 100ms keeps the spinner smooth without making terminal writes noticeable.
constexpr auto kTickInterval = std::chrono::milliseconds(100);

constexpr std::string_view kSpinnerFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                              "⠴", "⠦", "⠧", "⠇", "⠏"};

constexpr std::string_view kAnsiReset = "\x1b[0m";
constexpr std::string_view kAnsiDim = "\x1b[2m";
constexpr std::string_view kAnsiGreen = "\x1b[32m";
constexpr std::string_view kAnsiCyan = "\x1b[36m";

std::string paint(std::string_view code, std::string_view text, bool color) {
  if (!color || text.empty()) {
    return std::string(text);
  }
  std::string out;
  out.reserve(code.size() + text.size() + kAnsiReset.size());
  out.append(code);
  out.append(text);
  out.append(kAnsiReset);
  return out;
}

std::string pad_count(std::size_t value, std::size_t width) {
  std::string text = std::to_string(value);
  if (text.size() < width) {
    text.insert(text.begin(), width - text.size(), ' ');
  }
  return text;
}

// Present-participle status tag shown next to the bar, e.g. "compiling".
std::string_view status_tag(build::ActionKind kind) {
  switch (kind) {
    case build::ActionKind::Compile:
      return "compiling";
    case build::ActionKind::Archive:
      return "archiving";
    case build::ActionKind::Link:
      return "linking";
    case build::ActionKind::Run:
      return "running";
    case build::ActionKind::Copy:
      return "copying";
    case build::ActionKind::Custom:
      break;
  }
  return "building";
}

// "compile src/foo.cpp" -> "src/foo.cpp", so the status tag is not repeated.
std::string target_of(const std::string& label, build::ActionKind kind) {
  const std::string_view view(label);
  const std::string_view verb = build::to_string(kind);
  if (view.size() > verb.size() && view.starts_with(verb) && view[verb.size()] == ' ') {
    return std::string(view.substr(verb.size() + 1));
  }
  return label;
}

// Columns occupied by UTF-8 text, ignoring continuation bytes. Labels are
// paths, so counting code points (not bytes) is enough here.
std::size_t display_width(std::string_view text) {
  std::size_t width = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0) != 0x80) {
      ++width;
    }
  }
  return width;
}

// Index of the code point starting at or before `index` - 1, walking backwards.
std::size_t previous_code_point(std::string_view text, std::size_t index) {
  if (index == 0) {
    return 0;
  }
  --index;
  while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
    --index;
  }
  return index;
}

// Keeps the informative tail of a long path visible: a terminal one column too
// narrow should show ".../manifest.cpp", not ".../src/manif".
std::string clip_left(std::string_view text, std::size_t width) {
  if (width == 0) {
    return {};
  }
  if (display_width(text) <= width) {
    return std::string(text);
  }
  const std::size_t keep = width > 3 ? width - 3 : width;
  std::size_t start = text.size();
  std::size_t kept = 0;
  while (start > 0 && kept < keep) {
    start = previous_code_point(text, start);
    ++kept;
  }
  return width > 3 ? "..." + std::string(text.substr(start))
                   : std::string(text.substr(start));
}

std::string spinner_frame(std::size_t frame) {
  return std::string(kSpinnerFrames[frame % std::size(kSpinnerFrames)]);
}

// Keeps the beginning of a message visible when the terminal is too narrow.
std::string clip_right(std::string_view text, std::size_t width) {
  if (width == 0) {
    return {};
  }
  if (display_width(text) <= width) {
    return std::string(text);
  }
  const std::size_t keep = width > 3 ? width - 3 : width;
  std::size_t used = 0;
  std::size_t index = 0;
  while (index < text.size() && used < keep) {
    ++index;
    while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
      ++index;
    }
    ++used;
  }
  return width > 3 ? std::string(text.substr(0, index)) + "..."
                   : std::string(text.substr(0, index));
}

// Repeats a (possibly multi-byte) glyph. `std::string(count, ch)` is wrong for
// anything outside the single-byte execution character set.
std::string repeat(std::string_view unit, std::size_t count) {
  std::string out;
  out.reserve(unit.size() * count);
  for (std::size_t index = 0; index < count; ++index) {
    out.append(unit);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// ProgressReporter — build actions
// ---------------------------------------------------------------------------

ProgressReporter::ProgressReporter(Ui& ui, Options options, bool enabled)
    : ui_(ui),
      options_(std::move(options)),
      enabled_(enabled),
      live_(enabled && !options_.verbose && ui.progress_live()),
      started_(std::chrono::steady_clock::now()),
      remaining_(options_.total) {
  if (!enabled_) {
    return;
  }
  if (!live_) {
    // A pipe cannot animate; the start-of-phase line is the useful signal.
    ui_.status(options_.check_label);
    return;
  }
  checking_ = true;
  ticker_ = std::jthread([this](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, kTickInterval, [this, &stop] {
        return stopped_ || stop.stop_requested();
      });
      if (stopped_ || stop.stop_requested()) {
        return;
      }
      // A single long compile must still look alive: repaint the spinner and
      // elapsed time even when no action started or finished in this tick.
      if (checking_ || header_printed_) {
        render_locked();
      }
    }
  });
}

ProgressReporter::~ProgressReporter() { stop(); }

void ProgressReporter::action_started(const build::Action& action) {
  if (!enabled_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) {
    return;
  }
  if (checking_) {
    checking_ = false;
    // Erase the "checking" line before the permanent build header is printed;
    // otherwise the header would land at the end of the transient line.
    if (live_) {
      ui_.progress_end();
    }
  }
  ensure_header_locked();
  running_.push_back(RunningAction{action.id.value, action.kind, action.label});
  last_label_ = action.label;
  last_kind_ = action.kind;
  if (live_) {
    render_locked();
  }
}

void ProgressReporter::action_finished(const build::Action& action,
                                       const build::ActionOutcome& outcome) {
  if (!enabled_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) {
    return;
  }
  const auto running = std::find_if(
      running_.begin(), running_.end(),
      [&](const RunningAction& entry) { return entry.id == action.id.value; });
  if (running != running_.end()) {
    running_.erase(running);
  }
  if (outcome.skipped) {
    // Up-to-date and cache-restored actions are removed from the denominator
    // instead of being counted as progress: an incremental build should show
    // the work it actually performs, and stay quiet about the rest.
    if (remaining_ > 0) {
      --remaining_;
    }
    return;
  }
  last_label_ = action.label;
  last_kind_ = action.kind;
  ++completed_;
  ensure_header_locked();
  if (live_) {
    render_locked();
    return;
  }
  const std::size_t digits = std::to_string(remaining_).size();
  std::string line = "[" + pad_count(completed_, digits) + "/" +
                     std::to_string(remaining_) + "] " + action.label + "  " +
                     format_duration(outcome.duration);
  if (!outcome.succeeded) {
    line += "  FAILED";
  }
  ui_.status(line);
}

void ProgressReporter::stop() {
  bool clear_line = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    clear_line = live_ && (header_printed_ || checking_);
  }
  wake_.notify_all();
  ticker_.request_stop();
  if (ticker_.joinable()) {
    ticker_.join();
  }
  if (clear_line) {
    ui_.progress_end();
  }
}

void ProgressReporter::ensure_header_locked() {
  if (header_printed_) {
    return;
  }
  header_printed_ = true;
  if (!options_.header.empty()) {
    ui_.status(options_.header);
  }
}

void ProgressReporter::render_locked() {
  if (!live_) {
    return;
  }
  ++frame_;
  ui_.progress_update(line_locked());
}

std::string ProgressReporter::line_locked() const {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_);
  const int columns = std::max(20, terminal_width());

  // Before the first action runs the executor is hashing inputs and consulting
  // the cache: animate that phase so a slow incremental check still looks alive.
  if (!header_printed_) {
    const std::string spinner = spinner_frame(frame_);
    const std::string timing = "  " + format_duration(elapsed);
    const std::size_t prefix_width = display_width("  " + spinner + " ");
    const std::size_t timing_width = display_width(timing);
    const std::size_t budget =
        static_cast<std::size_t>(columns) > prefix_width + timing_width
            ? static_cast<std::size_t>(columns) - prefix_width - timing_width
            : 0;
    const std::string label = clip_right(options_.check_label, budget);
    return "  " + paint(kAnsiCyan, spinner, options_.use_color) + " " + label + timing;
  }

  const std::size_t digits = std::to_string(remaining_).size();

  std::string plain;
  std::string colored;
  auto append = [&](std::string_view text, std::string_view code = {}) {
    plain.append(text);
    if (options_.use_color && !code.empty()) {
      colored.append(code).append(text).append(kAnsiReset);
    } else {
      colored.append(text);
    }
  };

  const RunningAction* current = running_.empty() ? nullptr : &running_.back();
  const build::ActionKind kind = current != nullptr ? current->kind : last_kind_;
  const std::string& label = current != nullptr ? current->label : last_label_;

  append("  ");
  append(spinner_frame(frame_), kAnsiCyan);
  append(" ");

  // Task count first, then the bar: a filled segment followed by the remaining
  // work, so the line reads as progress at a glance.
  const std::string counts =
      pad_count(completed_, digits) + "/" + std::to_string(remaining_);
  append(counts);
  append("  ");

  // The bar is dropped on narrow terminals; count, elapsed time, status and the
  // current target always survive.
  if (columns >= 55) {
    const std::size_t bar_width =
        static_cast<std::size_t>(std::clamp(columns / 6, 10, 22));
    const std::size_t filled =
        remaining_ == 0 ? bar_width
                        : std::min(bar_width, (completed_ * bar_width) / remaining_);
    if (filled > 0) {
      append(repeat("━", filled), kAnsiGreen);
    }
    if (filled < bar_width) {
      append(repeat("─", bar_width - filled), kAnsiDim);
    }
    append("  ");
  }

  append(format_duration(elapsed));
  append("  ");
  // Status label: what the build is doing right now.
  append(status_tag(kind), kAnsiCyan);
  append("  ");
  if (running_.size() > 1 && columns >= 90) {
    const std::string parallel = std::to_string(running_.size()) + " running  ";
    append(parallel);
  }

  std::string target = target_of(label, kind);
  if (target.empty()) {
    target = "starting";
  }
  const std::size_t used = display_width(plain);
  target = used < static_cast<std::size_t>(columns)
               ? clip_left(target, static_cast<std::size_t>(columns) - used)
               : std::string();
  colored += target;
  return colored;
}

// ---------------------------------------------------------------------------
// StageProgress — preparation stages
// ---------------------------------------------------------------------------

StageProgress::StageProgress(Ui& ui, bool enabled, bool verbose, bool use_color)
    : ui_(ui),
      enabled_(enabled),
      use_color_(use_color),
      live_(enabled && !verbose && ui.progress_live()) {}

StageProgress::~StageProgress() { cancel(); }

void StageProgress::begin(std::string label) {
  if (!enabled_) {
    return;
  }
  cancel();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    label_ = std::move(label);
    started_ = std::chrono::steady_clock::now();
    frame_ = 0;
    stop_requested_ = false;
    active_ = true;
  }
  if (!live_) {
    // A pipe cannot animate; one line at the start is more useful in a log than
    // a line printed only after the wait.
    ui_.status(label_);
    return;
  }
  ticker_ = std::jthread([this](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, kTickInterval, [this, &stop] {
        return stop_requested_ || stop.stop_requested();
      });
      if (stop_requested_ || stop.stop_requested()) {
        return;
      }
      lock.unlock();
      render();
    }
  });
}

void StageProgress::finish() {
  if (!enabled_ || !active_) {
    return;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_);
  stop_ticker();
  active_ = false;
  if (live_) {
    ui_.progress_end();
    ui_.status(label_ + "  (" + format_duration(elapsed) + ")");
  }
}

void StageProgress::cancel() {
  if (!enabled_) {
    return;
  }
  const bool was_active = active_;
  stop_ticker();
  active_ = false;
  if (was_active && live_) {
    ui_.progress_end();
  }
}

void StageProgress::render() {
  std::string label;
  std::chrono::milliseconds elapsed{0};
  std::size_t frame = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      return;
    }
    label = label_;
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_);
    frame = ++frame_;
  }

  const int columns = std::max(20, terminal_width());
  const std::string spinner = spinner_frame(frame);
  const std::string timing = "  " + format_duration(elapsed);
  const std::string prefix = "  " + spinner + " ";
  const std::size_t prefix_width = display_width(prefix);
  const std::size_t timing_width = display_width(timing);
  const std::size_t budget =
      static_cast<std::size_t>(columns) > prefix_width + timing_width
          ? static_cast<std::size_t>(columns) - prefix_width - timing_width
          : 0;
  // Stage names matter more than the target at their end, so keep the start.
  label = clip_right(label, budget);

  std::string line = "  " + paint(kAnsiCyan, spinner, use_color_) + " " + label + timing;
  ui_.progress_update(line);
}

void StageProgress::stop_ticker() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  wake_.notify_all();
  ticker_.request_stop();
  if (ticker_.joinable()) {
    ticker_.join();
  }
}

}  // namespace cova::cli
