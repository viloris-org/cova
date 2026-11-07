// Cova — child process execution (PRD §7.6, §9.4, §13.3).
//
// No shell is involved: arguments are passed as an argv vector, so paths with
// spaces and shell metacharacters are safe by construction.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova::proc {

using Path = std::filesystem::path;

struct ProcessSpec {
  Path program;                    // absolute or resolved via PATH
  std::vector<std::string> args;   // argv[1..]
  std::optional<Path> cwd;         // working directory
  std::vector<std::pair<std::string, std::string>> env_overrides;  // added/replaced
  std::vector<std::string> env_remove;
  std::optional<std::string> stdin_data;
  std::chrono::milliseconds timeout{0};  // 0 = no timeout
  bool inherit_stdout = false;           // stream straight to our stdout
  bool inherit_stderr = false;
  // Forward our stdin to the child (needed for interactive `cova run`).
  bool inherit_stdin = false;
};

struct ProcessResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string stdout_text;
  std::string stderr_text;
  std::chrono::milliseconds duration{0};

  bool success() const { return exit_code == 0 && !timed_out; }
  // Trimmed stdout, useful for version probes.
  std::string stdout_trimmed() const;
  std::string stderr_trimmed() const;
  std::string to_string() const;
};

// Runs to completion and captures output.
Result<ProcessResult> run(const ProcessSpec& spec);

// Convenience wrappers.
Result<ProcessResult> run(const Path& program, const std::vector<std::string>& args,
                          const std::optional<Path>& cwd = std::nullopt);
// Convenience overload taking a command-line string that is split with quote
// awareness (see str::split_whitespace_quoted).
Result<ProcessResult> run_line(const std::string& command_line);

// Runs and returns stdout when the process exits 0, otherwise an Error that
// carries the exit code and stderr tail.
Result<std::string> capture(const Path& program, const std::vector<std::string>& args,
                            const std::optional<Path>& cwd = std::nullopt);

// Returns true if the program exists and is executable.
bool is_executable(const Path& program);

// Renders argv for logging: quotes arguments containing spaces.
std::string format_command_line(const ProcessSpec& spec);

}  // namespace cova::proc
