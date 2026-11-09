// Cova — child process execution tests (PRD §7.6, §9.4).
//
// The backend is platform-specific, so each case drives a standard shell that
// exists on the host. A missing shell skips rather than fails, but on the CI
// hosts (Linux and Windows) these cases exercise the real fork/exec and
// CreateProcess paths.
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "core/process/environment.hpp"
#include "core/process/process.hpp"
#include "testing/test.hpp"

namespace {

struct Shell {
  std::string program;
  std::string flag;
  bool available = false;
};

Shell host_shell() {
#ifdef _WIN32
  if (cova::fs::find_in_path("cmd.exe").has_value()) {
    return Shell{"cmd.exe", "/c", true};
  }
  return Shell{"cmd.exe", "/c", false};
#else
  if (cova::fs::find_in_path("sh").has_value()) {
    return Shell{"sh", "-c", true};
  }
  return Shell{"sh", "-c", false};
#endif
}

cova::proc::ProcessSpec shell_spec(const Shell& shell, const std::string& command) {
  cova::proc::ProcessSpec spec;
  spec.program = shell.program;
  spec.args = {shell.flag, command};
  spec.timeout = std::chrono::milliseconds(10000);
  return spec;
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

COVA_TEST_NAMED(core, process_captures_stdout_and_stderr_separately) {
  const Shell shell = host_shell();
  if (!shell.available) {
    return;
  }
#ifdef _WIN32
  auto result = COVA_REQUIRE_OK(cova::proc::run(
      shell_spec(shell, "echo out & echo err 1>&2 & exit /b 7")));
#else
  auto result = COVA_REQUIRE_OK(
      cova::proc::run(shell_spec(shell, "echo out; echo err >&2; exit 7")));
#endif
  COVA_CHECK_EQ(result.exit_code, 7);
  COVA_CHECK(!result.timed_out);
  COVA_CHECK_MSG(contains(result.stdout_text, "out"), result.stdout_text);
  COVA_CHECK_MSG(contains(result.stderr_text, "err"), result.stderr_text);
  COVA_CHECK(!contains(result.stdout_text, "err"));
}

COVA_TEST_NAMED(core, process_honours_the_working_directory) {
  const Shell shell = host_shell();
  if (!shell.available) {
    return;
  }
  cova::test::TempDir temp;
#ifdef _WIN32
  auto spec = shell_spec(shell, "cd");
#else
  auto spec = shell_spec(shell, "pwd");
#endif
  spec.cwd = cova::fs::Path(temp.path());
  auto result = COVA_REQUIRE_OK(cova::proc::run(spec));
  COVA_CHECK(result.success());
  // The temp directory's unique name must appear in the reported path.
  const std::string name = cova::fs::Path(temp.path()).filename().string();
  COVA_CHECK_MSG(contains(result.stdout_text, name), result.stdout_text);
}

COVA_TEST_NAMED(core, process_feeds_stdin) {
  const Shell shell = host_shell();
  if (!shell.available) {
    return;
  }
#ifdef _WIN32
  auto spec = shell_spec(shell, "findstr .");
#else
  auto spec = shell_spec(shell, "cat");
#endif
  spec.stdin_data = std::string("hello from stdin\n");
  auto result = COVA_REQUIRE_OK(cova::proc::run(spec));
  COVA_CHECK(result.success());
  COVA_CHECK_MSG(contains(result.stdout_text, "hello from stdin"), result.stdout_text);
}

COVA_TEST_NAMED(core, process_timeout_terminates_the_child) {
  const Shell shell = host_shell();
  if (!shell.available) {
    return;
  }
#ifdef _WIN32
  if (!cova::fs::find_in_path("ping.exe").has_value()) {
    return;
  }
  auto spec = shell_spec(shell, "ping -n 10 127.0.0.1 > nul");
#else
  auto spec = shell_spec(shell, "sleep 10");
#endif
  spec.timeout = std::chrono::milliseconds(100);
  auto result = COVA_REQUIRE_OK(cova::proc::run(spec));
  COVA_CHECK(result.timed_out);
  COVA_CHECK_EQ(result.exit_code, -1);
}

COVA_TEST_NAMED(core, process_applies_environment_overrides) {
  const Shell shell = host_shell();
  if (!shell.available) {
    return;
  }
#ifdef _WIN32
  auto spec = shell_spec(shell, "echo %COVA_PROC_TEST%");
#else
  auto spec = shell_spec(shell, "printf '%s' \"$COVA_PROC_TEST\"");
#endif
  spec.env_overrides.emplace_back("COVA_PROC_TEST", "visible");
  auto result = COVA_REQUIRE_OK(cova::proc::run(spec));
  COVA_CHECK(result.success());
  COVA_CHECK_MSG(contains(result.stdout_text, "visible"), result.stdout_text);
}

COVA_TEST_NAMED(core, process_reports_a_missing_program) {
  auto result = cova::proc::run(
      cova::fs::Path("cova-definitely-not-a-program"),
      {"--version"}, std::nullopt);
  COVA_CHECK(!result.has_value());
  COVA_CHECK_EQ(result.error().code, std::string("cova::proc::not_found"));
}

COVA_TEST_NAMED(core, process_explicit_environment_hides_parent_variables) {
  const Shell shell = host_shell();
  if (!shell.available) {
    return;
  }
  cova::test::ScopedEnv leaked("COVA_PARENT_ONLY", "leaked-value");
  cova::proc::EnvironmentRequest request;
  request.additions.push_back(
      cova::proc::EnvironmentVariable{"COVA_EXPLICIT_VISIBLE", "visible-value", true});
  auto environment = COVA_REQUIRE_OK(cova::proc::collect_environment(request));
  auto spec = shell_spec(shell,
#ifdef _WIN32
                         "echo %COVA_EXPLICIT_VISIBLE%:%COVA_PARENT_ONLY%"
#else
                         "printf '%s:%s' \"$COVA_EXPLICIT_VISIBLE\" \"$COVA_PARENT_ONLY\""
#endif
  );
  spec.environment = environment.variables();
  auto result = COVA_REQUIRE_OK(cova::proc::run(spec));
  COVA_CHECK(result.success());
  COVA_CHECK_MSG(contains(result.stdout_text, "visible-value"), result.stdout_text);
  COVA_CHECK_MSG(!contains(result.stdout_text, "leaked-value"), result.stdout_text);
}

COVA_TEST_MAIN()
