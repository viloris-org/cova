// Cova — child process execution (implementation).
//
// POSIX: fork/execvp with pipes. No shell is ever involved, so argv entries are
// passed literally and never re-split (PRD §7.6, §9.4).
//
// Windows ships a stub that returns honest "unsupported" errors; Cova's Milestone
// targets are Linux/macOS hosts, and pretending to run a process there would be
// worse than failing loudly.
#include "core/process/process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common.hpp"
#include "core/filesystem/filesystem.hpp"

#ifdef _WIN32

namespace cova::proc {

std::string ProcessResult::stdout_trimmed() const { return str::trim(stdout_text); }
std::string ProcessResult::stderr_trimmed() const { return str::trim(stderr_text); }

std::string ProcessResult::to_string() const {
  return "exit=" + std::to_string(exit_code) + (timed_out ? " (timed out)" : "");
}

Result<ProcessResult> run(const ProcessSpec& spec) {
  return std::unexpected(
      Error::make("cova::proc::unsupported", "process execution is not implemented on Windows")
          .with("program: " + spec.program.string()));
}

Result<ProcessResult> run(const Path& program, const std::vector<std::string>& args,
                          const std::optional<Path>& cwd) {
  ProcessSpec spec;
  spec.program = program;
  spec.args = args;
  spec.cwd = cwd;
  return run(spec);
}

Result<ProcessResult> run_line(const std::string& command_line) {
  std::vector<std::string> parts = str::split_whitespace_quoted(command_line);
  if (parts.empty()) {
    return std::unexpected(Error::make("cova::proc::empty", "empty command line"));
  }
  Path program = parts.front();
  parts.erase(parts.begin());
  return run(program, parts, std::nullopt);
}

Result<std::string> capture(const Path& program, const std::vector<std::string>& args,
                            const std::optional<Path>& cwd) {
  auto result = run(program, args, cwd);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  if (!result->success()) {
    return std::unexpected(Error::make("cova::proc::failed", "command failed")
                               .with("exit_code: " + std::to_string(result->exit_code))
                               .with("stderr: " + result->stderr_trimmed()));
  }
  return result->stdout_text;
}

bool is_executable(const Path& program) {
  return fs::is_regular_file(program);
}

std::string format_command_line(const ProcessSpec& spec) {
  std::string out = spec.program.string();
  for (const std::string& arg : spec.args) {
    out.push_back(' ');
    out += arg.find(' ') == std::string::npos ? arg : str::quote(arg);
  }
  return out;
}

}  // namespace cova::proc

#else  // POSIX

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace cova::proc {
namespace {

constexpr std::chrono::milliseconds kPollSlice{20};

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

// Builds the environment block for execve from the parent environment plus the
// spec's overrides/removals. Kept deterministic in ordering.
std::vector<std::string> build_environment(const ProcessSpec& spec) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (char** env = environ; env != nullptr && *env != nullptr; ++env) {
    const std::string_view line(*env);
    const std::size_t equals = line.find('=');
    const std::string name(line.substr(0, equals));
    const std::string value = equals == std::string_view::npos
                                  ? std::string()
                                  : std::string(line.substr(equals + 1));
    entries.emplace_back(name, value);
  }

  auto find = [&entries](std::string_view name) -> auto {
    return std::find_if(entries.begin(), entries.end(),
                        [name](const auto& entry) { return entry.first == name; });
  };

  for (const std::string& removed : spec.env_remove) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&removed](const auto& entry) { return entry.first == removed; }),
                  entries.end());
  }
  for (const auto& [name, value] : spec.env_overrides) {
    auto it = find(name);
    if (it == entries.end()) {
      entries.emplace_back(name, value);
    } else {
      it->second = value;
    }
  }

  std::vector<std::string> block;
  block.reserve(entries.size());
  for (const auto& [name, value] : entries) {
    block.push_back(name + "=" + value);
  }
  return block;
}

// Resolves the program: an explicit path is used verbatim (must be executable),
// a bare name is looked up on PATH before forking so the error is precise.
Result<std::string> resolve_program(const Path& program) {
  const std::string text = program.string();
  if (text.empty()) {
    return std::unexpected(Error::make("cova::proc::invalid", "empty program path"));
  }
  if (text.find('/') != std::string::npos) {
    if (!fs::is_regular_file(program)) {
      return std::unexpected(Error::make("cova::proc::not_found", "program does not exist")
                                 .with("program: " + text));
    }
    if (::access(text.c_str(), X_OK) != 0) {
      return std::unexpected(Error::make("cova::proc::not_executable", "program is not executable")
                                 .with("program: " + text));
    }
    return text;
  }
  const std::optional<Path> found = fs::find_in_path(text);
  if (!found) {
    return std::unexpected(Error::make("cova::proc::not_found", "program not found on PATH")
                               .with("program: " + text));
  }
  return found->string();
}

}  // namespace

std::string ProcessResult::stdout_trimmed() const { return str::trim(stdout_text); }
std::string ProcessResult::stderr_trimmed() const { return str::trim(stderr_text); }

std::string ProcessResult::to_string() const {
  std::string out = "exit=" + std::to_string(exit_code);
  if (timed_out) {
    out += " timed_out";
  }
  out += " duration=" + std::to_string(duration.count()) + "ms";
  return out;
}

Result<ProcessResult> run(const ProcessSpec& spec) {
  auto resolved = resolve_program(spec.program);
  if (!resolved) {
    return std::unexpected(std::move(resolved.error()));
  }

  // Validate the working directory before forking so the caller gets a precise
  // error rather than a generic exec failure.
  if (spec.cwd && !fs::is_directory(*spec.cwd)) {
    return std::unexpected(Error::make("cova::proc::cwd", "working directory does not exist")
                               .with("cwd: " + spec.cwd->string()));
  }

  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int stdin_pipe[2] = {-1, -1};
  bool needs_stdin_pipe = spec.stdin_data.has_value() && !spec.inherit_stdin;

  if (!spec.inherit_stdout) {
    if (::pipe(stdout_pipe) != 0) {
      return std::unexpected(Error::make("cova::proc::pipe", "cannot create stdout pipe")
                                 .with("errno: " + std::string(std::strerror(errno))));
    }
  }
  if (!spec.inherit_stderr) {
    if (::pipe(stderr_pipe) != 0) {
      close_fd(stdout_pipe[0]);
      close_fd(stdout_pipe[1]);
      return std::unexpected(Error::make("cova::proc::pipe", "cannot create stderr pipe")
                                 .with("errno: " + std::string(std::strerror(errno))));
    }
  }
  if (needs_stdin_pipe) {
    if (::pipe(stdin_pipe) != 0) {
      close_fd(stdout_pipe[0]);
      close_fd(stdout_pipe[1]);
      close_fd(stderr_pipe[0]);
      close_fd(stderr_pipe[1]);
      return std::unexpected(Error::make("cova::proc::pipe", "cannot create stdin pipe")
                                 .with("errno: " + std::string(std::strerror(errno))));
    }
  }

  // Everything the child needs must be owned by this frame; the child only reads.
  std::vector<std::string> environment = build_environment(spec);
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (std::string& entry : environment) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  std::vector<std::string> argv_storage;
  argv_storage.push_back(*resolved);
  for (const std::string& arg : spec.args) {
    argv_storage.push_back(arg);
  }
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (std::string& arg : argv_storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  const auto started = std::chrono::steady_clock::now();
  const pid_t pid = ::fork();
  if (pid < 0) {
    close_fd(stdout_pipe[0]);
    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[0]);
    close_fd(stderr_pipe[1]);
    close_fd(stdin_pipe[0]);
    close_fd(stdin_pipe[1]);
    return std::unexpected(Error::make("cova::proc::fork", "cannot fork")
                               .with("errno: " + std::string(std::strerror(errno))));
  }

  if (pid == 0) {
    // --- child ---
    // Own process group so a timeout can SIGKILL the whole tree it spawns.
    ::setpgid(0, 0);

    if (spec.cwd && ::chdir(spec.cwd->c_str()) != 0) {
      ::_exit(127);
    }

    if (spec.inherit_stdin) {
      // Our stdin is already the child's stdin.
    } else if (needs_stdin_pipe) {
      ::dup2(stdin_pipe[0], STDIN_FILENO);
      close_fd(stdin_pipe[0]);
      close_fd(stdin_pipe[1]);
    } else {
      const int devnull = ::open("/dev/null", O_RDONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::close(devnull);
      }
    }

    if (!spec.inherit_stdout) {
      ::dup2(stdout_pipe[1], STDOUT_FILENO);
      close_fd(stdout_pipe[0]);
      close_fd(stdout_pipe[1]);
    }
    if (!spec.inherit_stderr) {
      ::dup2(stderr_pipe[1], STDERR_FILENO);
      close_fd(stderr_pipe[0]);
      close_fd(stderr_pipe[1]);
    }

    ::execve(argv[0], argv.data(), envp.data());
    // If exec fails, report through the dedicated exit code.
    ::_exit(127);
  }

  // --- parent ---
  ::setpgid(pid, pid);  // race-free with the child's own setpgid call
  close_fd(stdout_pipe[1]);
  close_fd(stderr_pipe[1]);
  close_fd(stdin_pipe[0]);

  // Non-blocking read ends: `drain` must return at EAGAIN instead of blocking,
  // otherwise a chatty child that never closes its pipes would deadlock us.
  auto set_nonblocking = [](int fd) {
    if (fd < 0) {
      return;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  };
  set_nonblocking(stdout_pipe[0]);
  set_nonblocking(stderr_pipe[0]);

  // Streaming stdin: bounded writes driven by POLLOUT so a large payload can
  // never block the parent while the child is still starting up.
  std::size_t stdin_offset = 0;
  const std::size_t stdin_size = spec.stdin_data ? spec.stdin_data->size() : 0;
  bool stdin_closed = !needs_stdin_pipe;
  if (!needs_stdin_pipe) {
    close_fd(stdin_pipe[1]);
  }
  auto pump_stdin = [&]() {
    if (stdin_closed || stdin_pipe[1] < 0) {
      return;
    }
    while (stdin_offset < stdin_size) {
      const std::size_t remaining = stdin_size - stdin_offset;
      const std::size_t chunk = std::min<std::size_t>(remaining, 65536);
      const ssize_t n = ::write(stdin_pipe[1], spec.stdin_data->data() + stdin_offset, chunk);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;  // EPIPE: the child is not reading; that is the child's choice
      }
      stdin_offset += static_cast<std::size_t>(n);
      break;  // one bounded chunk per readiness notification
    }
    if (stdin_offset >= stdin_size) {
      close_fd(stdin_pipe[1]);
      stdin_closed = true;
    }
  };

  ProcessResult result;
  bool timed_out = false;
  bool child_exited = false;
  int wait_status = 0;
  std::array<char, 65536> buffer{};
  const auto deadline = spec.timeout.count() > 0 ? started + spec.timeout
                                                 : std::chrono::steady_clock::time_point::max();

  auto drain = [&](int fd, std::string& sink) {
    for (;;) {
      const ssize_t n = ::read(fd, buffer.data(), buffer.size());
      if (n > 0) {
        sink.append(buffer.data(), static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;  // EOF or EAGAIN
    }
  };

  while (!child_exited) {
    if (spec.timeout.count() > 0 && std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      ::kill(-pid, SIGKILL);
      ::kill(pid, SIGKILL);
      break;
    }

    std::array<struct pollfd, 3> fds{};
    nfds_t count = 0;
    int stdout_slot = -1;
    int stderr_slot = -1;
    int stdin_slot = -1;
    if (stdout_pipe[0] >= 0) {
      stdout_slot = static_cast<int>(count);
      fds[count++] = {stdout_pipe[0], static_cast<short>(POLLIN), 0};
    }
    if (stderr_pipe[0] >= 0) {
      stderr_slot = static_cast<int>(count);
      fds[count++] = {stderr_pipe[0], static_cast<short>(POLLIN), 0};
    }
    if (!stdin_closed && stdin_pipe[1] >= 0) {
      stdin_slot = static_cast<int>(count);
      fds[count++] = {stdin_pipe[1], static_cast<short>(POLLOUT), 0};
    }

    std::chrono::milliseconds wait_for = kPollSlice;
    if (spec.timeout.count() > 0) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining < wait_for) {
        wait_for = remaining.count() > 0 ? remaining : std::chrono::milliseconds(0);
      }
    }

    const int ready = ::poll(fds.data(), count, static_cast<int>(wait_for.count()));
    if (ready < 0 && errno != EINTR) {
      break;
    }
    if (ready > 0) {
      if (stdout_slot >= 0 && (fds[stdout_slot].revents & (POLLIN | POLLHUP)) != 0) {
        drain(stdout_pipe[0], result.stdout_text);
        if ((fds[stdout_slot].revents & POLLHUP) != 0) {
          drain(stdout_pipe[0], result.stdout_text);
          close_fd(stdout_pipe[0]);
        }
      }
      if (stderr_slot >= 0 && (fds[stderr_slot].revents & (POLLIN | POLLHUP)) != 0) {
        drain(stderr_pipe[0], result.stderr_text);
        if ((fds[stderr_slot].revents & POLLHUP) != 0) {
          drain(stderr_pipe[0], result.stderr_text);
          close_fd(stderr_pipe[0]);
        }
      }
      if (stdin_slot >= 0 && (fds[stdin_slot].revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
        pump_stdin();
      }
    } else {
      // No file descriptors ready: the child made no progress, so check whether
      // it is gone. `waitpid` with WNOHANG never blocks.
      pid_t waited = ::waitpid(pid, &wait_status, WNOHANG);
      if (waited == pid) {
        child_exited = true;
        break;
      }
    }
  }

  if (!child_exited) {
    // Either we timed out, or the pipes closed before the process exited.
    for (;;) {
      const pid_t waited = ::waitpid(pid, &wait_status, 0);
      if (waited == pid) {
        child_exited = true;
        break;
      }
      if (waited < 0 && errno != EINTR) {
        break;
      }
    }
  }

  // Collect whatever output remains buffered in the pipes.
  if (stdout_pipe[0] >= 0) {
    drain(stdout_pipe[0], result.stdout_text);
    close_fd(stdout_pipe[0]);
  }
  if (stderr_pipe[0] >= 0) {
    drain(stderr_pipe[0], result.stderr_text);
    close_fd(stderr_pipe[0]);
  }
  close_fd(stdin_pipe[1]);

  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  if (timed_out) {
    result.timed_out = true;
    result.exit_code = -1;
    return result;
  }

  if (WIFEXITED(wait_status)) {
    result.exit_code = WEXITSTATUS(wait_status);
    if (result.exit_code == 127) {
      return std::unexpected(Error::make("cova::proc::exec", "failed to execute program")
                                 .with("program: " + *resolved)
                                 .with("stderr: " + result.stderr_trimmed()));
    }
  } else if (WIFSIGNALED(wait_status)) {
    result.exit_code = 128 + WTERMSIG(wait_status);
  } else {
    result.exit_code = -1;
  }
  return result;
}

Result<ProcessResult> run(const Path& program, const std::vector<std::string>& args,
                          const std::optional<Path>& cwd) {
  ProcessSpec spec;
  spec.program = program;
  spec.args = args;
  spec.cwd = cwd;
  return run(spec);
}

Result<ProcessResult> run_line(const std::string& command_line) {
  std::vector<std::string> parts = str::split_whitespace_quoted(command_line);
  if (parts.empty()) {
    return std::unexpected(Error::make("cova::proc::empty", "empty command line"));
  }
  Path program = parts.front();
  parts.erase(parts.begin());
  return run(program, parts, std::nullopt);
}

Result<std::string> capture(const Path& program, const std::vector<std::string>& args,
                            const std::optional<Path>& cwd) {
  auto result = run(program, args, cwd);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  if (!result->success()) {
    Error error = Error::make("cova::proc::failed", "command failed");
    ProcessSpec described;
    described.program = program;
    described.args = args;
    error.with("command: " + format_command_line(described));
    error.with("exit_code: " + std::to_string(result->exit_code));
    if (!result->stderr_text.empty()) {
      error.with("stderr: " + result->stderr_trimmed());
    }
    return std::unexpected(std::move(error));
  }
  return result->stdout_text;
}

bool is_executable(const Path& program) {
  if (!fs::is_regular_file(program)) {
    return false;
  }
  return ::access(program.c_str(), X_OK) == 0;
}

std::string format_command_line(const ProcessSpec& spec) {
  std::string out = spec.program.string();
  for (const std::string& arg : spec.args) {
    out.push_back(' ');
    if (arg.find(' ') != std::string::npos || arg.find('\t') != std::string::npos) {
      out += str::quote(arg);
    } else {
      out += arg;
    }
  }
  return out;
}

}  // namespace cova::proc

#endif  // _WIN32
