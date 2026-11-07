// Cova — user-facing output abstraction (PRD §19.3, §4).
#include <iostream>
#include <string>
#include <string_view>

#include "cli/cli.hpp"

namespace cova::cli {
namespace {

constexpr std::string_view kAnsiReset = "\x1b[0m";
constexpr std::string_view kAnsiBold = "\x1b[1m";
constexpr std::string_view kAnsiDim = "\x1b[2m";
constexpr std::string_view kAnsiRed = "\x1b[31m";
constexpr std::string_view kAnsiYellow = "\x1b[33m";
constexpr std::string_view kAnsiGreen = "\x1b[32m";
constexpr std::string_view kAnsiCyan = "\x1b[36m";

std::string wrap(std::string_view color_on, std::string_view text, bool color,
                 std::string_view color_off = kAnsiReset) {
  if (!color) {
    return std::string(text);
  }
  std::string out;
  out.reserve(color_on.size() + text.size() + color_off.size());
  out.append(color_on);
  out.append(text);
  out.append(color_off);
  return out;
}

// Ensures the stream has a trailing newline for the piece we just printed.
void write_line(std::ostream& os, std::string_view prefix, std::string_view text) {
  if (!prefix.empty()) {
    os << prefix;
  }
  os << text;
  if (text.empty() || text.back() != '\n') {
    os << '\n';
  }
  os.flush();
}

}  // namespace

StreamUi::StreamUi(std::ostream& out_stream, std::ostream& err_stream, bool color, bool verbose,
                   bool quiet)
    : out_(out_stream), err_(err_stream), color_(color), verbose_(verbose), quiet_(quiet) {}

void StreamUi::out(std::string_view text) { write_line(out_, {}, text); }

void StreamUi::info(std::string_view text) {
  if (quiet_) {
    return;
  }
  write_line(out_, wrap(kAnsiCyan, "  ", color_), text);
}

void StreamUi::warn(std::string_view text) {
  if (quiet_) {
    // Warnings are user-relevant; quiet only silences status lines.
    write_line(err_, wrap(kAnsiYellow, "warning: ", color_), text);
    return;
  }
  write_line(err_, wrap(kAnsiYellow, "warning: ", color_), text);
}

void StreamUi::error(std::string_view text) {
  write_line(err_, wrap(kAnsiRed, "error: ", color_, kAnsiReset), text);
}

void StreamUi::verbose(std::string_view text) {
  if (!verbose_) {
    return;
  }
  write_line(err_, wrap(kAnsiDim, "debug: ", color_), text);
}

bool StreamUi::confirm(std::string_view question, bool fallback) {
  if (!interactive_) {
    return fallback;
  }
  err_ << question << (fallback ? " [Y/n] " : " [y/N] ");
  err_.flush();
  std::string answer;
  if (!std::getline(std::cin, answer)) {
    return fallback;
  }
  if (answer.empty()) {
    return fallback;
  }
  char first = answer.front();
  return first == 'y' || first == 'Y';
}

// ---------------------------------------------------------------------------
// StringUi — collected output for tests
// ---------------------------------------------------------------------------
void StringUi::out(std::string_view text) {
  stdout_text.append(text);
  if (text.empty() || text.back() != '\n') {
    stdout_text.push_back('\n');
  }
}

void StringUi::info(std::string_view text) {
  stdout_text.append("  ").append(text);
  if (text.empty() || text.back() != '\n') {
    stdout_text.push_back('\n');
  }
}

void StringUi::warn(std::string_view text) {
  stderr_text.append("warning: ").append(text);
  if (text.empty() || text.back() != '\n') {
    stderr_text.push_back('\n');
  }
}

void StringUi::error(std::string_view text) {
  stderr_text.append("error: ").append(text);
  if (text.empty() || text.back() != '\n') {
    stderr_text.push_back('\n');
  }
}

void StringUi::verbose(std::string_view text) {
  stderr_text.append("debug: ").append(text);
  if (text.empty() || text.back() != '\n') {
    stderr_text.push_back('\n');
  }
}

bool StringUi::confirm(std::string_view question, bool fallback) {
  confirmations.emplace_back(question);
  if (!is_interactive) {
    return fallback;
  }
  return answer;
}

}  // namespace cova::cli
