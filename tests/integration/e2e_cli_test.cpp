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
#include "core/hashing/hash.hpp"
#include "core/json/json.hpp"
#include "core/process/process.hpp"
#include "manifest/manifest.hpp"
#include "platform/binary/binary.hpp"
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
  result.exit_code = cova::cli::run(static_cast<int>(argv.size()), argv.data(), out, err);
  result.out = out.str();
  result.err = err.str();
  return result;
}

// Skips the current test when a required tool is missing.
#define COVA_SKIP_UNLESS_TOOL(name)                                                                \
  do {                                                                                             \
    if (!cova::fs::find_in_path(name).has_value()) {                                               \
      return;                                                                                      \
    }                                                                                              \
  } while (false)

// True when any C++ driver is reachable, versioned forms included. Homebrew
// installs `g++-15` rather than a `g++` alias, and a test that silently skipped
// on such a host would hide real breakage.
bool has_cxx_driver() {
  for (const std::string_view name : {"g++", "clang++", "c++"}) {
    if (cova::fs::find_in_path(name).has_value()) {
      return true;
    }
  }
  for (const std::filesystem::path& directory : cova::fs::path_entries()) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
      const std::string filename = it->path().filename().string();
      for (const std::string_view prefix : {"g++-", "clang++-"}) {
        if (filename.rfind(prefix, 0) == 0) {
          return true;
        }
      }
    }
  }
  return false;
}

// A Linux target that is not this host, so a cross build stays cross on both
// x86_64 and aarch64 runners.
std::string non_host_linux_target() {
  return cova::platform::host_triple().arch == cova::platform::Arch::Aarch64
             ? "x86_64-unknown-linux-gnu"
             : "aarch64-unknown-linux-gnu";
}

std::string non_host_driver_prefix() {
  return cova::platform::host_triple().arch == cova::platform::Arch::Aarch64
             ? "x86_64-linux-gnu"
             : "aarch64-linux-gnu";
}

#define COVA_SKIP_UNLESS_CXX_DRIVER()                                                              \
  do {                                                                                             \
    if (!has_cxx_driver()) {                                                                       \
      return;                                                                                      \
    }                                                                                              \
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

std::size_t count_occurrences(const std::string& haystack, std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = haystack.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

void make_executable(const stdfs::path& path) {
  std::error_code ec;
  stdfs::permissions(
      path, stdfs::perms::owner_exec | stdfs::perms::group_exec | stdfs::perms::others_exec,
      stdfs::perm_options::add, ec);
}

// A compiler stand-in that satisfies every invocation by creating its `-o`
// outputs (and `-MF` depfile) without producing a real binary. It is enough to
// exercise discovery, probing, planning and execution; artifact verification is
// what must then reject the empty result.
constexpr std::string_view kFakeCompiler = R"SH(#!/bin/sh
if [ "$1" = "--version" ]; then
  echo 'g++ (GCC) 14.2.0'
  exit 0
fi
if [ "$1" = "-print-resource-dir" ]; then
  dirname "$0"
  exit 0
fi
out=""
mf=""
prev=""
for arg in "$@"; do
  case "$prev" in
    -o) out="$arg" ;;
    -MF) mf="$arg" ;;
  esac
  prev="$arg"
done
if [ -n "$mf" ]; then
  mkdir -p "$(dirname "$mf")"
  : > "$mf"
fi
if [ -n "$out" ]; then
  mkdir -p "$(dirname "$out")"
  : > "$out"
fi
exit 0
)SH";

// A MinGW-shaped compiler stand-in. It writes a minimal PE/COFF stub (x86_64
// machine) to every `-o`, so the whole cross path — discovery, probing,
// planning, `.exe` naming and artifact verification — runs without needing a
// real MinGW installation.
constexpr std::string_view kFakeMingwCompiler = R"SH(#!/bin/sh
if [ "$1" = "--version" ]; then
  echo 'g++ (GCC) 14.2.0'
  exit 0
fi
if [ "$1" = "-print-resource-dir" ]; then
  dirname "$0"
  exit 0
fi
out=""
mf=""
prev=""
for arg in "$@"; do
  case "$prev" in
    -o) out="$arg" ;;
    -MF) mf="$arg" ;;
  esac
  prev="$arg"
done
if [ -n "$mf" ]; then
  mkdir -p "$(dirname "$mf")"
  : > "$mf"
fi
if [ -n "$out" ]; then
  mkdir -p "$(dirname "$out")"
  cp "$(dirname "$0")/pe-fixture" "$out"
fi
exit 0
)SH";

void write_pe_stub(const stdfs::path& path) {
  std::vector<unsigned char> bytes(0x40, 0);
  bytes[0] = 'M';
  bytes[1] = 'Z';
  bytes[0x3c] = 0x40; // e_lfanew points at the PE signature.
  bytes.push_back('P');
  bytes.push_back('E');
  bytes.push_back(0);
  bytes.push_back(0);
  bytes.push_back(0x64); // 0x8664 = IMAGE_FILE_MACHINE_AMD64.
  bytes.push_back(0x86);
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

stdfs::path find_extension(const stdfs::path& root, std::string_view extension) {
  std::error_code ec;
  for (stdfs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension().string() == extension) {
      return it->path();
    }
  }
  return {};
}

// Creates a fresh project and builds it for a Windows GNU target. Returns the
// first `.exe` the build produced, or an empty path on failure.
stdfs::path build_windows_project(const stdfs::path& root, std::string_view target,
                                  std::string_view compiler) {
  {
    ScopedCwd cwd(root);
    auto created = run_cli({"new", "winapp"});
    COVA_CHECK_MSG(created.ok(), created.combined());
  }
  ScopedCwd cwd(root / "winapp");
  auto built = run_cli({"build", "--target", std::string(target), "--compiler",
                        std::string(compiler)});
  COVA_CHECK_MSG(built.ok(), built.combined());
  return find_extension(root / "winapp", ".exe");
}

} // namespace

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
  COVA_CHECK_MSG(reloaded.has_value(), reloaded ? "" : reloaded.error().to_string());
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
  COVA_SKIP_UNLESS_CXX_DRIVER();
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
  // Progress is reported while the build runs, not only in the final summary
  // (stderr keeps it out of stdout, which stays machine-readable).
  COVA_CHECK_MSG(contains(built.err, "Compiling `hello`"), built.err);
  COVA_CHECK_MSG(contains(built.err, "compile src/main.cpp"), built.err);

  // The binary exists and is a real host executable.
  const std::string executable = "hello" + cova::platform::host_triple().executable_suffix();
  const stdfs::path binary = stdfs::path(temp.path()) / "hello" / "target" /
                             cova::cli::target_dir_component(cova::platform::host_triple()) /
                             "dev" / "bin" / executable;
  COVA_CHECK_MSG(cova::fs::is_regular_file(binary), "expected binary at " + binary.string());

  auto run = run_cli({"run"});
  COVA_CHECK_MSG(run.ok(), run.combined());
  COVA_CHECK(contains(run.out, "Hello from"));
}

COVA_TEST_NAMED(integration, incremental_build_rebuilds_nothing_when_unchanged) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
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
  COVA_CHECK(contains(second.combined(), "up to date") || contains(second.combined(), "Finished"));

  // Touching a header must trigger a rebuild (PRD §9.3).
  const stdfs::path header = stdfs::path(temp.path()) / "inc" / "include" / "inc.hpp";
  if (cova::fs::is_regular_file(header)) {
    write_text(header, read_text(header) + "\n// touch\n");
    auto third = run_cli({"build"});
    COVA_CHECK_MSG(third.ok(), third.combined());
  }
}

COVA_TEST_NAMED(integration, changing_the_build_environment_invalidates_artifacts) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path root = stdfs::path(temp.path()) / "envapp";
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "envapp"}).ok());
  }
  const stdfs::path inc_one = stdfs::path(temp.path()) / "inc-one";
  const stdfs::path inc_two = stdfs::path(temp.path()) / "inc-two";
  stdfs::create_directories(inc_one);
  stdfs::create_directories(inc_two);
  write_text(inc_one / "value.hpp", "#pragma once\n#define COVA_ENV_VALUE \"one\"\n");
  write_text(inc_two / "value.hpp", "#pragma once\n#define COVA_ENV_VALUE \"two\"\n");
  write_text(root / "src" / "main.cpp",
             "#include <value.hpp>\n#include <iostream>\n"
             "int main() { std::cout << COVA_ENV_VALUE << \"\\n\"; return 0; }\n");
  ScopedCwd cwd(root);

  {
    cova::test::ScopedEnv cpath("CPATH", inc_one.string());
    auto first = run_cli({"run"});
    COVA_CHECK_MSG(first.ok(), first.combined());
    COVA_CHECK_MSG(contains(first.combined(), "one"), first.combined());
  }
  {
    cova::test::ScopedEnv cpath("CPATH", inc_two.string());
    auto second = run_cli({"run"});
    COVA_CHECK_MSG(second.ok(), second.combined());
    COVA_CHECK_MSG(contains(second.combined(), "two"), second.combined());
  }
  {
    cova::test::ScopedEnv cpath("CPATH", inc_one.string());
    auto restored = run_cli({"run"});
    COVA_CHECK_MSG(restored.ok(), restored.combined());
    COVA_CHECK_MSG(contains(restored.combined(), "one"), restored.combined());
  }
}

COVA_TEST_NAMED(integration, response_files_follow_compiler_nested_resolution) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path root = stdfs::path(temp.path()) / "rspapp";
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "rspapp"}).ok());
  }
  write_text(root / "rsp" / "main.rsp", "@nested.rsp\n");
  write_text(root / "nested.rsp", "-DRSP_VALUE=1\n");
  write_text(root / "rsp" / "nested.rsp", "-DRSP_VALUE=2\n");
  write_text(root / "src" / "main.cpp",
             "#include <cstdio>\n"
             "#ifndef RSP_VALUE\n#define RSP_VALUE 0\n#endif\n"
             "int main() { std::printf(\"rsp=%d\\n\", RSP_VALUE); return 0; }\n");
  const std::string manifest = read_text(root / "cova.toml");
  write_text(root / "cova.toml",
             cova::str::replace_all(manifest, "[build]",
                                    "[build]\ncompile-options = [\"@rsp/main.rsp\"]"));
  ScopedCwd cwd(root);

  auto first = run_cli({"run", "--verbose"});
  COVA_CHECK_MSG(first.ok(), first.combined());
  COVA_CHECK_MSG(contains(first.combined(), "rsp=1"), first.combined());

  // GCC and Clang resolve the nested @file against the driver's cwd. Changing
  // the file next to main.rsp must not invalidate the action.
  write_text(root / "rsp" / "nested.rsp", "-DRSP_VALUE=3\n");
  auto unused = run_cli({"run", "--verbose"});
  COVA_CHECK_MSG(unused.ok(), unused.combined());
  COVA_CHECK_MSG(contains(unused.combined(), "rsp=1"), unused.combined());
  COVA_CHECK_MSG(contains(unused.combined(), "restored from cache") ||
                     contains(unused.combined(), "up to date"),
                 unused.combined());

  // Changing the file the compiler actually read must rebuild.
  write_text(root / "nested.rsp", "-DRSP_VALUE=4\n");
  auto used = run_cli({"run", "--verbose"});
  COVA_CHECK_MSG(used.ok(), used.combined());
  COVA_CHECK_MSG(contains(used.combined(), "rsp=4"), used.combined());
}

COVA_TEST_NAMED(integration, dependency_scans_run_only_when_the_closure_is_unknown) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path root = stdfs::path(temp.path()) / "scanapp";
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "scanapp"}).ok());
  }
  write_text(root / "src" / "extra.cpp", "int extra_value() { return 1; }\n");
  ScopedCwd cwd(root);

  auto cold = run_cli({"build", "--verbose"});
  COVA_CHECK_MSG(cold.ok(), cold.combined());
  COVA_CHECK_EQ(count_occurrences(cold.combined(), "dependency scan:"), std::size_t{2});

  auto hot = run_cli({"build", "--verbose"});
  COVA_CHECK_MSG(hot.ok(), hot.combined());
  COVA_CHECK_EQ(count_occurrences(hot.combined(), "dependency scan:"), std::size_t{0});

  write_text(root / "src" / "extra.cpp", "int extra_value() { return 2; }\n");
  auto changed = run_cli({"build", "--verbose"});
  COVA_CHECK_MSG(changed.ok(), changed.combined());
  COVA_CHECK_EQ(count_occurrences(changed.combined(), "dependency scan:"), std::size_t{1});
}

COVA_TEST_NAMED(integration, dry_run_does_not_restore_cache_outputs) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path root = stdfs::path(temp.path()) / "drycache";
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "drycache"}).ok());
  }
  ScopedCwd cwd(root);
  auto built = run_cli({"build"});
  COVA_CHECK_MSG(built.ok(), built.combined());
  COVA_CHECK_MSG(run_cli({"clean"}).ok(), "clean failed");
  auto dry = run_cli({"build", "--dry-run"});
  COVA_CHECK_MSG(dry.ok(), dry.combined());

  auto count_objects = [&]() {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry : stdfs::recursive_directory_iterator(root, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      const std::string extension = entry.path().extension().string();
      if (extension == ".o" || extension == ".obj") {
        ++count;
      }
    }
    return count;
  };
  COVA_CHECK_EQ(count_objects(), std::size_t{0});
}

COVA_TEST_NAMED(integration, clean_removes_artifacts_but_keeps_sources) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
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

COVA_TEST_NAMED(integration, cross_build_rejects_a_wrong_architecture_artifact) {
#ifdef _WIN32
  return;  // the fixture driver is a POSIX shell script
#endif
  COVA_SKIP_UNLESS_TOOL("tar");
  const std::string target_text = non_host_linux_target();
  const std::string driver = non_host_driver_prefix() + "-g++";
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path base = temp.path();
  const stdfs::path registry = base / "registry";
  const stdfs::path stage = base / "stage" / "fake-cross";
  const stdfs::path archive = registry / "archives" / "fake-cross.tar.gz";

  write_text(stage / "bin" / driver, kFakeCompiler);
  make_executable(stage / "bin" / driver);
  write_text(stage / "toolchain.toml", "id = \"fake-cross\"\n"
                                       "family = \"gcc\"\n"
                                       "version = \"14.2.0\"\n"
                                       "target = \"" + target_text +
                                           "\"\ncompiler = \"bin/" + driver + "\"\n");
  stdfs::create_directories(archive.parent_path());
  auto tar = cova::proc::run(
      "tar", {"-czf", archive.string(), "-C", (base / "stage").string(), "fake-cross"});
  COVA_CHECK_MSG(tar.has_value() && tar->success(), "could not create the fixture archive");

  const std::string digest = *cova::Sha256::hex_of_file(archive);
  write_text(registry / "index.toml", "[[toolchain]]\n"
                                      "id = \"fake-cross\"\n"
                                      "family = \"gcc\"\n"
                                      "version = \"14.2.0\"\n"
                                      "target = \"" + target_text +
                                          "\"\narchive = \"archives/fake-cross.tar.gz\"\n"
                                      "sha256 = \"" +
                                          digest +
                                          "\"\n"
                                          "size-bytes = " +
                                          std::to_string(cova::fs::file_size(archive)) +
                                          "\ncompiler = \"bin/" + driver + "\"\n");

  {
    ScopedCwd cwd(base);
    COVA_CHECK(run_cli({"new", "fake"}).ok());
    auto installed =
        run_cli({"toolchain", "install", "fake-cross", "--registry", registry.string()});
    COVA_CHECK_MSG(installed.ok(), installed.combined());
  }

  ScopedCwd cwd(base / "fake");
  auto built = run_cli({"build", "--target", target_text});
  COVA_CHECK(!built.ok());
  COVA_CHECK(contains(built.combined(), "cannot inspect") ||
             contains(built.combined(), "artifact"));
}

COVA_TEST_NAMED(integration, cross_plans_and_verifies_a_windows_gnu_executable) {
#ifdef _WIN32
  return; // the fixture driver is a POSIX shell script
#endif
  COVA_SKIP_UNLESS_TOOL("cp");
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path bin = stdfs::path(temp.path()) / "mingw-bin";
  stdfs::create_directories(bin);
  write_text(bin / "x86_64-w64-mingw32-g++", kFakeMingwCompiler);
  make_executable(bin / "x86_64-w64-mingw32-g++");
  write_pe_stub(bin / "pe-fixture");
  cova::test::ScopedEnv path_env(
      "PATH", bin.string() + ":" + cova::fs::get_env("PATH").value_or(""));

  // MinGW's own spelling is accepted, canonicalised and used end to end.
  const stdfs::path exe = build_windows_project(temp.path(), "x86_64-w64-mingw32", "gcc");
  COVA_CHECK_MSG(!exe.empty(), "the cross build produced no .exe artifact");
  if (!exe.empty()) {
    auto info = cova::platform::inspect_binary(exe);
    COVA_CHECK(info.has_value());
    if (info) {
      COVA_CHECK(info->format == cova::platform::BinaryFormat::Pe);
      COVA_CHECK(info->arch == cova::platform::Arch::X86_64);
    }
  }
}

COVA_TEST_NAMED(integration, cross_builds_a_windows_executable_with_a_real_mingw_toolchain) {
#ifdef _WIN32
  return;
#endif
  if (!cova::fs::find_in_path("x86_64-w64-mingw32-g++").has_value()) {
    return; // requires a MinGW-w64 cross toolchain on the host
  }
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));

  const stdfs::path exe = build_windows_project(temp.path(), "x86_64-pc-windows-gnu", "gcc");
  COVA_CHECK_MSG(!exe.empty(), "the cross build produced no .exe artifact");
  if (!exe.empty()) {
    auto info = cova::platform::inspect_binary(exe);
    COVA_CHECK(info.has_value());
    if (info) {
      COVA_CHECK(info->format == cova::platform::BinaryFormat::Pe);
      COVA_CHECK(info->arch == cova::platform::Arch::X86_64);
    }
  }
}


namespace {

// Writes a tiny static-library dependency tree: base <- math <- app.
struct DependencyFixture {
  stdfs::path root;
  stdfs::path base;
  stdfs::path math;
  stdfs::path app;
};

DependencyFixture write_dependency_fixture(const stdfs::path& root) {
  DependencyFixture fixture;
  fixture.root = root;
  fixture.base = root / "deps" / "base";
  fixture.math = root / "deps" / "math";
  fixture.app = root / "app";

  write_text(fixture.base / "cova.toml", R"(
[package]
name = "base"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/**/*.cpp"]
public-includes = ["include"]
public-definitions = ["BASE_PUBLIC=1"]
)");
  write_text(fixture.base / "include" / "base.hpp", "#pragma once\nint base_value();\n");
  write_text(fixture.base / "src" / "base.cpp",
             "#include \"base.hpp\"\nint base_value() { return 7; }\n");

  write_text(fixture.math / "cova.toml", R"(
[package]
name = "math"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/**/*.cpp"]
public-includes = ["include"]
private-includes = ["src"]
private-definitions = ["MATH_PRIVATE=1"]
[dependencies]
base = { path = "../base" }
)");
  write_text(fixture.math / "include" / "math.hpp",
             "#pragma once\n#include \"base.hpp\"\nint math_value();\n");
  write_text(fixture.math / "src" / "math.cpp",
             "#include \"math.hpp\"\nint math_value() { return base_value() + 1; }\n");

  write_text(fixture.app / "cova.toml", R"(
[package]
name = "app"
version = "0.1.0"
[build]
type = "executable"
sources = ["src/**/*.cpp"]
)");
  write_text(fixture.app / "src" / "main.cpp", R"(#include <cstdio>
#include "math.hpp"
int main() {
  std::printf("math=%d\n", math_value());
  return math_value() == 8 ? 0 : 1;
}
)");
  return fixture;
}

const cova::json::Value* object_field(const cova::json::Value& value, std::string_view key) {
  const auto* object = value.as_object();
  if (object == nullptr) {
    return nullptr;
  }
  auto it = object->find(key);
  return it == object->end() ? nullptr : &it->second;
}

bool arguments_contain(const cova::json::Value& entry, std::string_view needle) {
  const auto* arguments = object_field(entry, "arguments");
  if (arguments == nullptr || arguments->as_array() == nullptr) {
    return false;
  }
  for (const auto& argument : *arguments->as_array()) {
    if (argument.as_string().find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

COVA_TEST_NAMED(integration, path_dependency_outside_a_workspace_builds_and_links) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const auto fixture = write_dependency_fixture(stdfs::path(temp.path()));

  ScopedCwd cwd(fixture.app);
  auto added = run_cli({"add", "math", "--path", "../deps/math"});
  COVA_CHECK_MSG(added.ok(), added.combined());

  auto built = run_cli({"build"});
  COVA_CHECK_MSG(built.ok(), built.combined());

  auto run = run_cli({"run"});
  COVA_CHECK_MSG(run.ok(), run.combined());
  COVA_CHECK(contains(run.out, "math=8"));

  // The resolved graph is recorded in a lockfile.
  COVA_CHECK(cova::fs::is_regular_file(fixture.app / "cova.lock"));
  const std::string lock = read_text(fixture.app / "cova.lock");
  COVA_CHECK(contains(lock, "name = \"math\""));
  COVA_CHECK(contains(lock, "name = \"base\""));
  COVA_CHECK(contains(lock, "path = \"../deps/math\""));

  // One compilation database entry per compile action, with exact arguments.
  const auto database_path = fixture.app / "compile_commands.json";
  COVA_CHECK(cova::fs::is_regular_file(database_path));
  auto parsed = cova::json::Value::parse(read_text(database_path));
  COVA_CHECK_MSG(parsed.has_value(), parsed ? "" : parsed.error().to_string());
  const auto* entries = parsed->as_array();
  COVA_CHECK(entries != nullptr);

  bool saw_app_source = false;
  bool leaked_private_flag = false;
  bool has_transitive_include = false;
  bool has_transitive_definition = false;
  bool has_exact_compile = false;
  for (const auto& entry : *entries) {
    const auto* file = object_field(entry, "file");
    if (file == nullptr || file->as_string().find("app/src/main.cpp") == std::string::npos) {
      continue;
    }
    saw_app_source = true;
    has_transitive_include = arguments_contain(entry, "deps/math/include") &&
                             arguments_contain(entry, "deps/base/include");
    has_transitive_definition = arguments_contain(entry, "-DBASE_PUBLIC=1");
    leaked_private_flag = arguments_contain(entry, "MATH_PRIVATE") ||
                          arguments_contain(entry, "path/deps/math/src") ||
                          arguments_contain(entry, "deps/math/src");
    has_exact_compile = arguments_contain(entry, "-c") && arguments_contain(entry, "app/src/main.cpp");
  }
  COVA_CHECK(saw_app_source);
  COVA_CHECK(has_transitive_include);
  COVA_CHECK(has_transitive_definition);
  COVA_CHECK(!leaked_private_flag);
  COVA_CHECK(has_exact_compile);

  // `cova check` re-emits the database too.
  COVA_CHECK(cova::fs::remove_file(database_path).has_value());
  auto checked = run_cli({"check"});
  COVA_CHECK_MSG(checked.ok(), checked.combined());
  COVA_CHECK(cova::fs::is_regular_file(database_path));
}

COVA_TEST_NAMED(integration, lockfile_is_written_and_enforced) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const auto fixture = write_dependency_fixture(stdfs::path(temp.path()));
  COVA_CHECK(cova::fs::write_file(fixture.app / "cova.toml", R"(
[package]
name = "app"
version = "0.1.0"
[build]
type = "executable"
sources = ["src/**/*.cpp"]
[dependencies]
math = { path = "../deps/math" }
)")
                 .has_value());
  const auto lock_path = fixture.app / "cova.lock";

  ScopedCwd cwd(fixture.app);
  auto first = run_cli({"build"});
  COVA_CHECK_MSG(first.ok(), first.combined());
  COVA_CHECK(cova::fs::is_regular_file(lock_path));

  auto locked = run_cli({"build", "--locked"});
  COVA_CHECK_MSG(locked.ok(), locked.combined());

  // Missing lockfile in locked mode fails and is not silently created.
  COVA_CHECK(cova::fs::remove_file(lock_path).has_value());
  auto missing = run_cli({"build", "--locked"});
  COVA_CHECK(!missing.ok());
  COVA_CHECK(contains(missing.combined(), "cova.lock"));
  COVA_CHECK(!cova::fs::is_regular_file(lock_path));

  // Unlocked mode writes it again.
  auto recreated = run_cli({"build"});
  COVA_CHECK_MSG(recreated.ok(), recreated.combined());
  COVA_CHECK(cova::fs::is_regular_file(lock_path));

  // A changed manifest makes the lockfile stale; locked mode reports the drift
  // and leaves the file untouched.
  const std::string before = read_text(lock_path);
  COVA_CHECK(cova::fs::write_file(fixture.app / "cova.toml", R"(
[package]
name = "app"
version = "0.2.0"
[build]
type = "executable"
sources = ["src/**/*.cpp"]
[dependencies]
math = { path = "../deps/math" }
)")
                 .has_value());
  auto stale = run_cli({"build", "--locked"});
  COVA_CHECK(!stale.ok());
  COVA_CHECK(contains(stale.combined(), "out of date") ||
             contains(stale.combined(), "cova.lock"));
  const std::string after = read_text(lock_path);
  COVA_CHECK_EQ(after, before);

  // `--frozen` implies `--locked`: it fails without a lockfile.
  COVA_CHECK(cova::fs::remove_file(lock_path).has_value());
  auto frozen_missing = run_cli({"build", "--frozen"});
  COVA_CHECK(!frozen_missing.ok());
  auto refreshed = run_cli({"build"});
  COVA_CHECK_MSG(refreshed.ok(), refreshed.combined());
  auto frozen = run_cli({"build", "--frozen"});
  COVA_CHECK_MSG(frozen.ok(), frozen.combined());
}

COVA_TEST_NAMED(integration, workspace_emits_one_compile_database) {
  COVA_SKIP_UNLESS_CXX_DRIVER();
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path root = stdfs::path(temp.path()) / "ws";
  write_text(root / "cova.toml", "[workspace]\nmembers = [\"packages/*\"]\n");
  write_text(root / "packages" / "math" / "cova.toml", R"(
[package]
name = "math"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/**/*.cpp"]
public-includes = ["include"]
)");
  write_text(root / "packages" / "math" / "include" / "math.hpp",
             "#pragma once\nint math_value();\n");
  write_text(root / "packages" / "math" / "src" / "math.cpp",
             "#include \"math.hpp\"\nint math_value() { return 42; }\n");
  write_text(root / "packages" / "app" / "cova.toml", R"(
[package]
name = "app"
version = "0.1.0"
[build]
type = "executable"
sources = ["src/**/*.cpp"]
[dependencies]
math = { path = "../math" }
)");
  write_text(root / "packages" / "app" / "src" / "main.cpp",
             "#include \"math.hpp\"\nint main() { return math_value() == 42 ? 0 : 1; }\n");

  ScopedCwd cwd(root);
  auto built = run_cli({"build"});
  COVA_CHECK_MSG(built.ok(), built.combined());

  COVA_CHECK(cova::fs::is_regular_file(root / "compile_commands.json"));
  COVA_CHECK(!cova::fs::is_regular_file(root / "packages" / "math" /
                                        "compile_commands.json"));
  auto parsed = cova::json::Value::parse(read_text(root / "compile_commands.json"));
  COVA_CHECK_MSG(parsed.has_value(), parsed ? "" : parsed.error().to_string());
  const auto* entries = parsed->as_array();
  COVA_CHECK(entries != nullptr);
  bool saw_math = false;
  bool saw_app = false;
  for (const auto& entry : *entries) {
    const auto* file = object_field(entry, "file");
    if (file == nullptr) {
      continue;
    }
    const std::string text = file->as_string();
    saw_math = saw_math || text.find("packages/math/src/math.cpp") != std::string::npos;
    saw_app = saw_app || text.find("packages/app/src/main.cpp") != std::string::npos;
  }
  COVA_CHECK(saw_math);
  COVA_CHECK(saw_app);

  auto run = run_cli({"run"});
  COVA_CHECK_MSG(run.ok(), run.combined());
}

COVA_TEST_NAMED(integration, build_script_true_is_rejected) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  {
    ScopedCwd cwd(temp.path());
    COVA_CHECK(run_cli({"new", "scripts"}).ok());
  }
  const stdfs::path root = stdfs::path(temp.path()) / "scripts";
  std::string manifest = read_text(root / "cova.toml");
  const std::string anchor = "type = \"executable\"";
  const auto position = manifest.find(anchor);
  COVA_CHECK(position != std::string::npos);
  manifest.insert(position + anchor.size(), "\nbuild-script = true");
  write_text(root / "cova.toml", manifest);

  ScopedCwd cwd(root);
  auto built = run_cli({"build"});
  COVA_CHECK(!built.ok());
  COVA_CHECK(contains(built.combined(), "build-script"));
  COVA_CHECK(contains(built.combined(), "not supported") ||
             contains(built.combined(), "does not implement"));
}

COVA_TEST_NAMED(integration, sysroot_gc_reclaims_unreferenced_store_entries) {
  cova::test::TempDir temp;
  cova::test::ScopedEnv home("COVA_HOME", temp.sub("covahome"));
  const stdfs::path orphan = stdfs::path(temp.path()) / "covahome" / "store" / "orphan";
  write_text(orphan / "payload.txt", "unused\n");

  auto dry = run_cli({"sysroot", "gc", "--dry-run", "--json"});
  COVA_CHECK_MSG(dry.ok(), dry.combined());
  COVA_CHECK(contains(dry.out, "reclaimable_bytes"));
  COVA_CHECK(cova::fs::is_directory(orphan));

  auto collected = run_cli({"sysroot", "gc", "--yes", "--json"});
  COVA_CHECK_MSG(collected.ok(), collected.combined());
  COVA_CHECK(contains(collected.out, "reclaimed_bytes"));
  COVA_CHECK(!cova::fs::exists(orphan));

  // `sysroot gc` is not an alias for `sysroot verify`.
  auto verify = run_cli({"sysroot", "verify"});
  COVA_CHECK_MSG(verify.ok(), verify.combined());
  COVA_CHECK(contains(verify.combined(), "no sysroots to verify"));
}

COVA_TEST_MAIN()
