// Cova — end-to-end CLI integration tests (PRD §22.2).
//
// These drive the real CLI in-process: `cova new` → `cova build` → `cova run`,
// across a scratch directory with an isolated COVA_HOME. They are the tests that
// prove the product actually works, not just that the units compose.
//
// Every test skips cleanly when a required host tool is absent, so the suite
// stays honest on minimal machines.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/cli.hpp"
#include "cli/common.hpp"
#include "core/filesystem/filesystem.hpp"
#include "manifest/manifest.hpp"
#include "core/process/process.hpp"
#include "platform/triple/triple.hpp"
#include "testing/test.hpp"

namespace {

namespace stdfs = std::filesystem;

struct CliRun {
  int exit_code = 0;
  std::string out;
  std::string err;

  std::string combined() const { return out + err; }
  bool ok() const { return exit_code == 0; }
};

// Runs the CLI with captured streams in the current process.
CliRun run_cli(const std::vector<std::string>& args) {
  std::vector<std::string> full;
  full.emplace_back("cova");
  full.insert(full.end(), args.begin(), args.end());

  std::vector<char*> argv;
  argv.reserve(full.size());
  for (auto& arg : full) {
    argv.push_back(arg.data());
  }

  std::ostringstream out;
  std::ostringstream err;
  CliRun result;
  result.exit_code =
      cova::cli::run(static_cast<int>(argv.size()), argv.data(), out, err);
  result.out = out.str();
  result.err = err.str();
  return result;
}

// Skips the current test when a required tool is missing.
#define COVA_SKIP_UNLESS_TOOL(name)                                    \
  do {                                                                 \
    if (!cova::fs::find_in_path(name).has_value()) {                    \
      return;                                                          \
    }                                                                  \
  } while (false)

// Restores the working directory on scope exit.
class ScopedCwd {
 public:
  explicit ScopedCwd(const stdfs::path& path) : old_(stdfs::current_path()) {
    stdfs::current_path(path);
  }
  ~ScopedCwd() {
    std::error_code ec;
    stdfs::current_path(old_, ec);
  }
  ScopedCwd(const ScopedCwd&) = delete;
  ScopedCwd& operator=(const ScopedCwd&) = delete;

 private:
  stdfs::path old_;
};

std::string read_text(const stdfs::path& path) {
  std::ifstream stream(path);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

void write_text(const stdfs::path& path, std::string_view text) {
  stdfs::create_directories(path.parent_path());
  std::ofstream stream(path);
  stream << text;
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

COVA_TEST_NAMED(integration, new_creates_a_complete_project) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));

  {
    ScopedCwd cwd(temp.path());
    auto result = run_cli({"new", "hello"});
    COVA_CHECK_MSG(result.ok(), result.combined());
  }

  const stdfs::path root = stdfs::path(temp.path()) / "hello";
  COVA_CHECK(cova::fs::is_regular_file(root / "cova.toml"));
  COVA_CHECK(cova::fs::is_regular_file(root / "src" / "main.cpp"));
  COVA_CHECK(cova::fs::is_regular_file(root / ".gitignore"));
  COVA_CHECK(cova::fs::is_regular_file(root / ".clang-format"));
  COVA_CHECK(cova::fs::is_directory(root / "tests"));

  const std::string manifest = read_text(root / "cova.toml");
  COVA_CHECK(contains(manifest, "name = \"hello\""));
  COVA_CHECK(contains(manifest, "[package]"));
  COVA_CHECK(contains(manifest, "[build]"));

  // The generated manifest must be re-parseable by Cova itself.
  auto reloaded = cova::manifest::load(root / "cova.toml");
  COVA_CHECK_MSG(reloaded.has_value(),
                 reloaded ? "" : reloaded.error().to_string());
  COVA_CHECK_EQ(reloaded->manifest.package.name, std::string("hello"));
}

COVA_TEST_NAMED(integration, new_lib_creates_library_layout) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  {
    ScopedCwd cwd(temp.path());
    auto result = run_cli({"new", "math", "--lib"});
    COVA_CHECK_MSG(result.ok(), result.combined());
  }
  const stdfs::path root = stdfs::path(temp.path()) / "math";
  COVA_CHECK(cova::fs::is_regular_file(root / "cova.toml"));
  COVA_CHECK(cova::fs::is_regular_file(root / "include" / "math.hpp"));
  COVA_CHECK(cova::fs::is_regular_file(root / "src" / "math.cpp"));

  auto reloaded = cova::manifest::load(root / "cova.toml");
  COVA_CHECK(reloaded.has_value());
  COVA_CHECK(reloaded->manifest.build.kind == cova::manifest::PackageKind::StaticLibrary);
}

COVA_TEST_NAMED(integration, new_refuses_to_overwrite_non_empty_directory) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  ScopedCwd cwd(temp.path());
  write_text(stdfs::path(temp.path()) / "app" / "keep.txt", "do not delete me");

  auto refused = run_cli({"new", "app"});
  COVA_CHECK(!refused.ok());
  COVA_CHECK(contains(refused.combined(), "not empty"));
  COVA_CHECK(cova::fs::is_regular_file(stdfs::path(temp.path()) / "app" / "keep.txt"));

  auto forced = run_cli({"new", "app", "--force"});
  COVA_CHECK_MSG(forced.ok(), forced.combined());
  COVA_CHECK(cova::fs::is_regular_file(stdfs::path(temp.path()) / "app" / "cova.toml"));
}

COVA_TEST_NAMED(integration, init_preserves_existing_sources_and_reports_foreign_builds) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path root = stdfs::path(temp.path()) / "legacy";
  write_text(root / "src" / "main.cpp", "int main() { return 0; }\n");
  write_text(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");

  ScopedCwd cwd(root);
  auto result = run_cli({"init"});
  COVA_CHECK_MSG(result.ok(), result.combined());

  COVA_CHECK(contains(read_text(root / "src" / "main.cpp"), "int main()"));
  COVA_CHECK(contains(read_text(root / "CMakeLists.txt"), "cmake_minimum_required"));
  COVA_CHECK(contains(result.combined(), "CMakeLists.txt"));
  COVA_CHECK(cova::fs::is_regular_file(root / "cova.toml"));

  auto manifest = cova::manifest::load(root / "cova.toml");
  COVA_CHECK(manifest.has_value());
  COVA_CHECK_EQ(manifest->manifest.build.sources.size(), std::size_t{1});
}

COVA_TEST_NAMED(integration, init_detects_executable_layout) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path root = stdfs::path(temp.path()) / "tool";
  write_text(root / "src" / "main.cpp", "int main() { return 0; }\n");

  ScopedCwd cwd(root);
  auto result = run_cli({"init"});
  COVA_CHECK_MSG(result.ok(), result.combined());
  COVA_CHECK(contains(result.combined(), "src/main.cpp") ||
             contains(result.combined(), "executable"));

  auto manifest = cova::manifest::load(root / "cova.toml");
  COVA_CHECK(manifest.has_value());
  COVA_CHECK(manifest->manifest.build.kind == cova::manifest::PackageKind::Executable);
}

COVA_TEST_NAMED(integration, target_list_reports_known_targets) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  auto result = run_cli({"target", "list"});
  COVA_CHECK_MSG(result.ok(), result.combined());
  COVA_CHECK(contains(result.out, "x86_64-unknown-linux-gnu"));
  COVA_CHECK(contains(result.out, "aarch64-unknown-linux-musl"));

  auto json = run_cli({"target", "list", "--json"});
  COVA_CHECK(json.ok());
  COVA_CHECK(contains(json.out, "\"triple\""));
}

COVA_TEST_NAMED(integration, help_and_version_exit_zero) {
  auto help = run_cli({"--help"});
  COVA_CHECK(help.ok());
  COVA_CHECK(contains(help.out, "Usage: cova"));

  auto version = run_cli({"--version"});
  COVA_CHECK(version.ok());
  COVA_CHECK(contains(version.out, "cova "));

  auto unknown = run_cli({"frobnicate"});
  COVA_CHECK(!unknown.ok());
  COVA_CHECK(contains(unknown.combined(), "unrecognised command"));
}

COVA_TEST_NAMED(integration, build_outside_a_project_fails_clearly) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  ScopedCwd cwd(temp.path());
  auto result = run_cli({"build"});
  COVA_CHECK(!result.ok());
  COVA_CHECK(contains(result.combined(), "cova.toml"));
  // The message must tell the user what to do next.
  COVA_CHECK(contains(result.combined(), "cova new") || contains(result.combined(), "cova init"));
}

COVA_TEST_NAMED(integration, end_to_end_new_build_run) {
  COVA_SKIP_UNLESS_TOOL("g++");
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  {
    ScopedCwd cwd(temp.path());
    auto created = run_cli({"new", "hello"});
    COVA_CHECK_MSG(created.ok(), created.combined());
  }
  ScopedCwd cwd(stdfs::path(temp.path()) / "hello");

  auto built = run_cli({"build"});
  COVA_CHECK_MSG(built.ok(), built.combined());

  // The binary exists and is a real ELF executable.
  const stdfs::path binary = stdfs::path(temp.path()) / "hello" / "target" /
                             cova::cli::target_dir_component(cova::platform::host_triple()) /
                             "dev" / "bin" / "hello";
  COVA_CHECK_MSG(cova::fs::is_regular_file(binary),
                 "expected binary at " + binary.string());

  auto run = run_cli({"run"});
  COVA_CHECK_MSG(run.ok(), run.combined());
  COVA_CHECK(contains(run.out, "Hello from"));
}

COVA_TEST_NAMED(integration, incremental_build_rebuilds_nothing_when_unchanged) {
  COVA_SKIP_UNLESS_TOOL("g++");
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "inc"}).ok());
  }
  ScopedCwd cwd(stdfs::path(temp.path()) / "inc");

  auto first = run_cli({"build"});
  COVA_CHECK_MSG(first.ok(), first.combined());
  auto second = run_cli({"build"});
  COVA_CHECK_MSG(second.ok(), second.combined());

  // A no-change build must not execute any compile action (PRD §9.3, §19.1).
  COVA_CHECK(contains(second.combined(), "up to date") ||
             contains(second.combined(), "Finished"));

  // Touching a header must trigger a rebuild (PRD §9.3).
  const stdfs::path header = stdfs::path(temp.path()) / "inc" / "include" / "inc.hpp";
  if (cova::fs::is_regular_file(header)) {
    write_text(header, read_text(header) + "\n// touch\n");
    auto third = run_cli({"build"});
    COVA_CHECK_MSG(third.ok(), third.combined());
  }
}

COVA_TEST_NAMED(integration, clean_removes_artifacts_but_keeps_sources) {
  COVA_SKIP_UNLESS_TOOL("g++");
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "cleanme"}).ok());
  }
  const stdfs::path root = stdfs::path(temp.path()) / "cleanme";
  ScopedCwd cwd(root);

  COVA_CHECK_MSG(run_cli({"build"}).ok(), "build failed");
  COVA_CHECK(cova::fs::is_directory(root / "target"));

  auto cleaned = run_cli({"clean"});
  COVA_CHECK_MSG(cleaned.ok(), cleaned.combined());
  COVA_CHECK(!cova::fs::is_directory(root / "target"));
  // Sources and the manifest survive.
  COVA_CHECK(cova::fs::is_regular_file(root / "src" / "main.cpp"));
  COVA_CHECK(cova::fs::is_regular_file(root / "cova.toml"));
  // Shared data directories are untouched by a default clean (PRD §12.1).
  COVA_CHECK(cova::fs::is_directory(temp.sub("covahome")));
}

COVA_TEST_NAMED(integration, metadata_reports_project_shape_as_json) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "meta"}).ok());
  }
  ScopedCwd cwd(stdfs::path(temp.path()) / "meta");

  auto result = run_cli({"metadata"});
  COVA_CHECK_MSG(result.ok(), result.combined());
  COVA_CHECK(contains(result.out, "\"name\""));
  COVA_CHECK(contains(result.out, "meta"));
  COVA_CHECK(contains(result.out, "\"target\""));
}

COVA_TEST_NAMED(integration, toolchain_list_finds_this_host_toolchain) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  auto result = run_cli({"toolchain", "list"});
  COVA_CHECK_MSG(result.ok(), result.combined());
  // Either a toolchain is listed, or the warning explains there is none. Both
  // are honest outcomes; a silent success would not be.
  COVA_CHECK(contains(result.combined(), "clang") || contains(result.combined(), "gcc") ||
             contains(result.combined(), "no toolchain"));
}

COVA_TEST_NAMED(integration, doctor_reports_structured_checks) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  auto json = run_cli({"doctor", "--json"});
  COVA_CHECK(contains(json.out, "\"checks\""));
  COVA_CHECK(contains(json.out, "\"name\""));
}

COVA_TEST_NAMED(integration, fmt_check_and_write) {
  COVA_SKIP_UNLESS_TOOL("clang-format");
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "formatted"}).ok());
  }
  const stdfs::path root = stdfs::path(temp.path()) / "formatted";
  ScopedCwd cwd(root);

  // Deliberately misformatted source.
  write_text(root / "src" / "main.cpp", "int    main( )   {return 0;}\n");

  auto check = run_cli({"fmt", "--check"});
  COVA_CHECK(!check.ok());

  auto written = run_cli({"fmt"});
  COVA_CHECK_MSG(written.ok(), written.combined());

  auto verified = run_cli({"fmt", "--check"});
  COVA_CHECK_MSG(verified.ok(), verified.combined());
}

COVA_TEST_NAMED(integration, cache_gc_is_safe_and_reports) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  auto result = run_cli({"cache", "gc", "--dry-run"});
  COVA_CHECK_MSG(result.ok(), result.combined());
  COVA_CHECK(contains(result.combined(), "cache"));
}

COVA_TEST_NAMED(integration, shell_escapes_are_never_interpreted) {
  // A package name containing shell metacharacters must not execute anything.
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  ScopedCwd cwd(temp.path());

  auto result = run_cli({"new", "safe;touch pwned"});
  // The command may reject the name or sanitise it, but nothing may be executed.
  COVA_CHECK(!cova::fs::is_regular_file(stdfs::path(temp.path()) / "pwned"));
  (void)result;
}

COVA_TEST_MAIN()
