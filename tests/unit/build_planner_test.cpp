// Cova — build graph, cache, depfile and planner tests (PRD §9).
#include <algorithm>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "build/cache/cache.hpp"
#include "build/graph/graph.hpp"
#include "build/planner.hpp"
#include "build/scheduler/scheduler.hpp"
#include "core/filesystem/filesystem.hpp"
#include "manifest/manifest.hpp"
#include "testing/test.hpp"

namespace {

cova::build::ActionEnvironment host_environment() {
  cova::build::ActionEnvironment environment;
  environment.target = cova::platform::host_triple();
  environment.abi.triple = environment.target;
  environment.abi.abi = environment.target.abi();
  environment.abi.crt = environment.target.crt();
  environment.abi.libcpp = environment.target.default_libcpp();
  environment.toolchain_fingerprint = "tc";
  environment.sysroot_fingerprint = "sys";
  return environment;
}

cova::toolchain::ResolvedToolchain dummy_toolchain(const std::string& archiver) {
  cova::toolchain::ResolvedToolchain resolved;
  resolved.descriptor.id = "dummy-gcc";
  resolved.descriptor.family = "gcc";
  resolved.descriptor.version = cova::Version{16, 0, 0};
  resolved.descriptor.host = cova::platform::host_triple();
  resolved.descriptor.source = cova::toolchain::ToolchainSource::System;
  cova::toolchain::ToolComponent cxx;
  cxx.role = "cxx";
  cxx.path = "/usr/bin/g++";
  resolved.descriptor.components.push_back(cxx);
  cova::toolchain::ToolComponent cc;
  cc.role = "cc";
  cc.path = "/usr/bin/gcc";
  resolved.descriptor.components.push_back(cc);
  if (!archiver.empty()) {
    cova::toolchain::ToolComponent ar;
    ar.role = "archiver";
    ar.path = archiver;
    resolved.descriptor.components.push_back(ar);
  }
  resolved.target = cova::platform::host_triple();
  resolved.abi_signature.triple = resolved.target;
  resolved.abi_signature.abi = resolved.target.abi();
  resolved.abi_signature.crt = resolved.target.crt();
  resolved.abi_signature.libcpp = resolved.target.default_libcpp();
  return resolved;
}

// Writes base <- math <- app into temp and returns the loaded manifests.
struct PathDependencyFixture {
  std::filesystem::path root;
  cova::manifest::Manifest base;
  cova::manifest::Manifest math;
  cova::manifest::Manifest app;
};

PathDependencyFixture make_path_dependency_fixture(const std::filesystem::path& root) {
  PathDependencyFixture fixture;
  fixture.root = root;
  const auto base_root = root / "deps" / "base";
  const auto math_root = root / "deps" / "math";
  const auto app_root = root / "app";
  COVA_CHECK(cova::fs::create_directories(base_root / "src").has_value());
  COVA_CHECK(cova::fs::create_directories(base_root / "include").has_value());
  COVA_CHECK(cova::fs::create_directories(math_root / "src").has_value());
  COVA_CHECK(cova::fs::create_directories(math_root / "include").has_value());
  COVA_CHECK(cova::fs::create_directories(app_root / "src").has_value());
  COVA_CHECK(cova::fs::write_file(base_root / "cova.toml", R"(
[package]
name = "base"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/*.cpp"]
public-includes = ["include"]
public-definitions = ["BASE_PUBLIC=1"]
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(math_root / "cova.toml", R"(
[package]
name = "math"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/*.cpp"]
public-includes = ["include"]
private-includes = ["src"]
private-definitions = ["MATH_PRIVATE=1"]
[dependencies]
base = { path = "../base" }
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(app_root / "cova.toml", R"(
[package]
name = "app"
version = "0.1.0"
[build]
type = "executable"
sources = ["src/*.cpp"]
[dependencies]
math = { path = "../deps/math" }
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(base_root / "src" / "base.cpp",
                                  "int base_value() { return 1; }\n")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(math_root / "src" / "math.cpp",
                                  "int math_value() { return 2; }\n")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(app_root / "src" / "main.cpp",
                                  "int main() { return 0; }\n")
                 .has_value());
  fixture.base = COVA_REQUIRE_OK(cova::manifest::load_from_root(base_root)).manifest;
  fixture.math = COVA_REQUIRE_OK(cova::manifest::load_from_root(math_root)).manifest;
  fixture.app = COVA_REQUIRE_OK(cova::manifest::load_from_root(app_root)).manifest;
  return fixture;
}

}  // namespace

COVA_TEST_NAMED(build, graph_orders_dependencies) {
  cova::build::BuildGraph graph;
  cova::build::Action compile;
  compile.label = "compile";
  const auto first = graph.add(compile);
  cova::build::Action link;
  link.label = "link";
  link.dependencies.push_back(first);
  graph.add(std::move(link));
  auto ordered = graph.actions_in_order();
  COVA_CHECK_EQ(ordered.size(), std::size_t{2});
  COVA_CHECK_EQ(ordered.front()->label, std::string("compile"));
  COVA_CHECK_EQ(ordered.back()->label, std::string("link"));
}

COVA_TEST_NAMED(build, cache_key_changes_with_input_contents) {
  cova::test::TempDir temp;
  const auto input = std::filesystem::path(temp.path()) / "input.cpp";
  COVA_CHECK(cova::fs::write_file(input, "int a = 1;\n").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.argv = {"/usr/bin/g++", "-c", cova::fs::generic_string(input)};
  action.inputs.push_back(cova::build::FileInput{input, "source"});
  const auto environment = host_environment();
  const std::string first = action.cache_key(environment);
  const std::string second = action.cache_key(environment);
  COVA_CHECK_EQ(first, second);
  COVA_CHECK(cova::fs::write_file(input, "int a = 2;\n").has_value());
  COVA_CHECK(first != action.cache_key(environment));
}

COVA_TEST_NAMED(build, state_round_trips_and_detects_changes) {
  cova::test::TempDir temp;
  const auto input = std::filesystem::path(temp.path()) / "input.cpp";
  const auto output = std::filesystem::path(temp.path()) / "out.o";
  COVA_CHECK(cova::fs::write_file(input, "int a = 1;\n").has_value());
  COVA_CHECK(cova::fs::write_file(output, "object\n").has_value());

  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.argv = {"cc", "-c", cova::fs::generic_string(input), "-o",
                 cova::fs::generic_string(output)};
  action.inputs.push_back(cova::build::FileInput{input, "source"});
  action.outputs.push_back(cova::build::FileOutput{output, true});
  auto environment = host_environment();
  auto key = action.cache_key(environment);

  cova::build::ActionRecord record;
  record.cache_key = key;
  record.output_paths.push_back(cova::fs::generic_string(output));
  record.output_hashes.emplace_back(
      cova::fs::generic_string(output),
      cova::to_hex(*cova::build::FileHasher{}.hash_file(output)));
  record.input_paths.push_back(cova::fs::generic_string(input));
  record.input_hashes.emplace_back(
      cova::fs::generic_string(input),
      cova::to_hex(*cova::build::FileHasher{}.hash_file(input)));
  record.succeeded = true;
  cova::build::BuildState state;
  state.record(std::move(record));
  const auto state_path = std::filesystem::path(temp.path()) / ".cova" / "state.toml";
  COVA_CHECK(state.save(state_path).has_value());

  auto reloaded = COVA_REQUIRE_OK(cova::build::BuildState::load(state_path));
  std::string reason;
  COVA_CHECK(reloaded.is_up_to_date(action, reason));
  COVA_CHECK(cova::fs::write_file(output, "corrupted object\n").has_value());
  COVA_CHECK(!reloaded.is_up_to_date(action, reason));
  COVA_CHECK(cova::fs::write_file(output, "object\n").has_value());
  COVA_CHECK(reloaded.is_up_to_date(action, reason));
  COVA_CHECK(cova::fs::write_file(input, "int a = 3;\n").has_value());
  COVA_CHECK(!reloaded.is_up_to_date(action, reason));
}

COVA_TEST_NAMED(build, parses_depfiles) {
  const std::string text = "obj/main.o: src/main.cpp include/a.hpp \\\n  include/b.hpp\n";
  auto depfile = COVA_REQUIRE_OK(
      cova::build::Depfile::parse(text, cova::fs::Path("/project")));
  COVA_CHECK_EQ(depfile.dependencies.size(), std::size_t{3});
  COVA_CHECK_EQ(depfile.dependencies.front(), cova::fs::Path("/project/src/main.cpp"));
}

COVA_TEST_NAMED(build, depfile_parser_does_not_split_windows_drive_letters) {
  const std::string text =
      "C:/build/obj/main.o: C:/src/main.cpp C:/src/include/a.hpp\n";
  auto depfile = COVA_REQUIRE_OK(
      cova::build::Depfile::parse(text, cova::fs::Path("/project")));
  COVA_CHECK_EQ(depfile.dependencies.size(), std::size_t{2});
  bool found_source = false;
  bool found_header = false;
  for (const auto& dependency : depfile.dependencies) {
    const std::string path = cova::fs::generic_string(dependency);
    found_source = found_source || path.find("C:/src/main.cpp") != std::string::npos;
    found_header = found_header || path.find("C:/src/include/a.hpp") != std::string::npos;
  }
  COVA_CHECK(found_source);
  COVA_CHECK(found_header);
}

COVA_TEST_NAMED(build, scheduler_releases_dependents) {
  cova::build::BuildGraph graph;
  cova::build::Action first;
  first.label = "first";
  auto first_id = graph.add(first);
  cova::build::Action second;
  second.label = "second";
  second.dependencies.push_back(first_id);
  auto second_id = graph.add(second);
  cova::build::Scheduler scheduler(graph);
  auto next = scheduler.next();
  COVA_CHECK(next.has_value());
  COVA_CHECK_EQ(next->value, first_id.value);
  COVA_CHECK(!scheduler.next().has_value());
  scheduler.complete(*next);
  auto after = scheduler.next();
  COVA_CHECK(after.has_value());
  COVA_CHECK_EQ(after->value, second_id.value);
}

COVA_TEST_NAMED(build, planner_emits_compile_and_link) {
  cova::test::TempDir temp;
  const auto root = std::filesystem::path(temp.path()) / "hello";
  COVA_CHECK(cova::fs::create_directories(root / "src").has_value());
  COVA_CHECK(cova::fs::write_file(root / "src" / "main.cpp",
                                  "int main() { return 0; }\n")
                 .has_value());
  auto manifest = cova::manifest::default_manifest("hello",
                                                   cova::manifest::PackageKind::Executable);
  manifest.path = root / "cova.toml";
  manifest.root = root;

  cova::build::PlanOptions options;
  options.target = cova::platform::host_triple();
  options.profile = "dev";
  auto plan = COVA_REQUIRE_OK(cova::build::plan_package(
      manifest, dummy_toolchain("/usr/bin/ar"), options,
      std::filesystem::path(temp.path()) / "target"));
  COVA_CHECK_EQ(plan.graph.size(), std::size_t{2});
  COVA_CHECK(plan.runnable_binary.has_value());
  COVA_CHECK(plan.runnable_binary->filename().string().find("hello") != std::string::npos);
  bool has_compile = false;
  bool has_link = false;
  for (const auto* action : plan.graph.actions_in_order()) {
    has_compile = has_compile || action->kind == cova::build::ActionKind::Compile;
    has_link = has_link || action->kind == cova::build::ActionKind::Link;
    COVA_CHECK(!action->argv.empty());
  }
  COVA_CHECK(has_compile);
  COVA_CHECK(has_link);
}

COVA_TEST_NAMED(build, workspace_links_path_dependencies) {
  cova::test::TempDir temp;
  const auto root = std::filesystem::path(temp.path()) / "ws";
  const auto math_root = root / "packages" / "math";
  const auto app_root = root / "packages" / "app";
  COVA_CHECK(cova::fs::create_directories(math_root / "src").has_value());
  COVA_CHECK(cova::fs::create_directories(math_root / "include").has_value());
  COVA_CHECK(cova::fs::create_directories(app_root / "src").has_value());
  COVA_CHECK(cova::fs::write_file(root / "cova.toml",
                                  "[workspace]\nmembers = [\"packages/*\"]\n")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(math_root / "cova.toml", R"(
[package]
name = "math"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/**/*.cpp"]
public-includes = ["include"]
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(math_root / "include" / "math.hpp",
                                  "int answer();\n")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(math_root / "src" / "math.cpp",
                                  "#include \"math.hpp\"\nint answer() { return 42; }\n")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(app_root / "cova.toml", R"(
[package]
name = "app"
version = "0.1.0"
[build]
type = "executable"
sources = ["src/**/*.cpp"]
[dependencies]
math = { path = "../math" }
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(app_root / "src" / "main.cpp",
                                  "#include \"math.hpp\"\nint main() { return answer(); }\n")
                 .has_value());

  auto root_manifest = COVA_REQUIRE_OK(cova::manifest::load_from_root(root));
  auto math_manifest = COVA_REQUIRE_OK(cova::manifest::load_from_root(math_root));
  auto app_manifest = COVA_REQUIRE_OK(cova::manifest::load_from_root(app_root));
  cova::build::WorkspaceProject workspace;
  workspace.root_manifest = root_manifest.manifest;
  workspace.members.push_back(math_manifest.manifest);
  workspace.members.push_back(app_manifest.manifest);

  cova::build::PlanOptions options;
  options.target = cova::platform::host_triple();
  options.profile = "dev";
  options.workspace = true;
  auto plan = COVA_REQUIRE_OK(cova::build::plan_workspace(
      workspace, dummy_toolchain("/usr/bin/ar"), options,
      std::filesystem::path(temp.path()) / "target"));

  bool app_has_math_include = false;
  bool app_links_math = false;
  for (const auto& action : plan.graph.actions()) {
    if (action.package != "app") {
      continue;
    }
    if (action.kind == cova::build::ActionKind::Compile) {
      for (const auto& argument : action.argv) {
        if (argument.find("math") != std::string::npos &&
            argument.rfind("-I", 0) == 0) {
          app_has_math_include = true;
        }
      }
    }
    if (action.kind == cova::build::ActionKind::Link) {
      for (const auto& argument : action.argv) {
        if (argument.find("libmath.a") != std::string::npos) {
          app_links_math = true;
        }
      }
    }
  }
  COVA_CHECK(app_has_math_include);
  COVA_CHECK(app_links_math);
}


COVA_TEST_NAMED(build, planner_propagates_public_interfaces_and_link_order) {
  cova::test::TempDir temp;
  auto fixture = make_path_dependency_fixture(temp.path());
  cova::build::WorkspaceProject project;
  project.members = {fixture.base, fixture.math, fixture.app};
  project.roots = {"app"};

  cova::build::PlanOptions options;
  options.target = cova::platform::host_triple();
  options.profile = "dev";
  auto plan = COVA_REQUIRE_OK(cova::build::plan_workspace(
      project, dummy_toolchain("/usr/bin/ar"), options,
      std::filesystem::path(temp.path()) / "target"));

  bool saw_app_compile = false;
  bool leaked_private = false;
  bool has_transitive_include = false;
  bool has_transitive_definition = false;
  bool saw_link = false;
  std::ptrdiff_t math_position = -1;
  std::ptrdiff_t base_position = -1;
  for (const auto& action : plan.graph.actions()) {
    if (action.package != "app") {
      continue;
    }
    if (action.kind == cova::build::ActionKind::Compile) {
      saw_app_compile = true;
      bool app_include = false;
      bool dep_include = false;
      bool base_include = false;
      for (const auto& argument : action.argv) {
        if (argument.find("app/src") != std::string::npos && argument.rfind("-I", 0) == 0) {
          app_include = true;
        }
        if (argument.find("deps/math/include") != std::string::npos) {
          dep_include = true;
        }
        if (argument.find("deps/base/include") != std::string::npos) {
          base_include = true;
        }
        if (argument.find("MATH_PRIVATE") != std::string::npos ||
            argument.find("deps/math/src") != std::string::npos) {
          leaked_private = true;
        }
        if (argument == "-DBASE_PUBLIC=1") {
          has_transitive_definition = true;
        }
      }
      (void)app_include;
      has_transitive_include = dep_include && base_include;
    }
    if (action.kind == cova::build::ActionKind::Link) {
      saw_link = true;
      for (std::size_t index = 0; index < action.argv.size(); ++index) {
        if (action.argv[index].find("libmath.a") != std::string::npos) {
          math_position = static_cast<std::ptrdiff_t>(index);
        }
        if (action.argv[index].find("libbase.a") != std::string::npos) {
          base_position = static_cast<std::ptrdiff_t>(index);
        }
      }
    }
  }
  COVA_CHECK(saw_app_compile);
  COVA_CHECK(saw_link);
  COVA_CHECK(has_transitive_include);
  COVA_CHECK(has_transitive_definition);
  COVA_CHECK(!leaked_private);
  // Static archives must be named dependents-first.
  COVA_CHECK(math_position >= 0);
  COVA_CHECK(base_position > math_position);

  COVA_CHECK(plan.runnable_binary.has_value());
  const std::string executable =
      std::string("app") + cova::platform::host_triple().executable_suffix();
  COVA_CHECK_EQ(plan.runnable_binary->filename().string(), executable);
  // Only the root contributes reported artifacts.
  COVA_CHECK_EQ(plan.expected_outputs.size(), std::size_t{1});
  COVA_CHECK(plan.expected_outputs.front().filename() == executable);
}

COVA_TEST_NAMED(build, dev_dependencies_are_only_wired_for_test_plans) {
  cova::test::TempDir temp;
  const auto math_root = std::filesystem::path(temp.path()) / "math";
  const auto app_root = std::filesystem::path(temp.path()) / "app";
  COVA_CHECK(cova::fs::create_directories(math_root / "src").has_value());
  COVA_CHECK(cova::fs::create_directories(math_root / "include").has_value());
  COVA_CHECK(cova::fs::create_directories(app_root / "src").has_value());
  COVA_CHECK(cova::fs::write_file(math_root / "cova.toml", R"(
[package]
name = "math"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/*.cpp"]
public-includes = ["include"]
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(app_root / "cova.toml", R"(
[package]
name = "app"
version = "0.1.0"
[build]
type = "executable"
sources = ["src/*.cpp"]
[dev-dependencies]
math = { path = "../math" }
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(math_root / "src" / "math.cpp", "int m();\n").has_value());
  COVA_CHECK(cova::fs::write_file(app_root / "src" / "main.cpp", "int main() { return 0; }\n")
                 .has_value());
  auto math = COVA_REQUIRE_OK(cova::manifest::load_from_root(math_root)).manifest;
  auto app = COVA_REQUIRE_OK(cova::manifest::load_from_root(app_root)).manifest;

  cova::build::WorkspaceProject project;
  project.members = {math, app};
  project.roots = {"app"};
  cova::build::PlanOptions options;
  options.target = cova::platform::host_triple();

  auto plain = COVA_REQUIRE_OK(cova::build::plan_workspace(
      project, dummy_toolchain("/usr/bin/ar"), options,
      std::filesystem::path(temp.path()) / "target"));
  for (const auto& action : plain.graph.actions()) {
    for (const auto& argument : action.argv) {
      COVA_CHECK(argument.find("math/include") == std::string::npos);
      COVA_CHECK(argument.find("libmath.a") == std::string::npos);
    }
  }

  options.include_tests = true;
  auto with_tests = COVA_REQUIRE_OK(cova::build::plan_workspace(
      project, dummy_toolchain("/usr/bin/ar"), options,
      std::filesystem::path(temp.path()) / "target"));
  bool wired = false;
  for (const auto& action : with_tests.graph.actions()) {
    for (const auto& argument : action.argv) {
      if (argument.find("math/include") != std::string::npos ||
          argument.find("libmath.a") != std::string::npos) {
        wired = true;
      }
    }
  }
  COVA_CHECK(wired);
}

COVA_TEST_NAMED(build, planner_rejects_dependency_cycles) {
  cova::test::TempDir temp;
  const auto first_root = std::filesystem::path(temp.path()) / "first";
  const auto second_root = std::filesystem::path(temp.path()) / "second";
  COVA_CHECK(cova::fs::create_directories(first_root / "src").has_value());
  COVA_CHECK(cova::fs::create_directories(second_root / "src").has_value());
  COVA_CHECK(cova::fs::write_file(first_root / "cova.toml", R"(
[package]
name = "first"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/*.cpp"]
[dependencies]
second = { path = "../second" }
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(second_root / "cova.toml", R"(
[package]
name = "second"
version = "0.1.0"
[build]
type = "static-library"
sources = ["src/*.cpp"]
[dependencies]
first = { path = "../first" }
)")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(first_root / "src" / "a.cpp", "int a();\\n").has_value());
  COVA_CHECK(cova::fs::write_file(second_root / "src" / "b.cpp", "int b();\\n").has_value());
  auto first = COVA_REQUIRE_OK(cova::manifest::load_from_root(first_root)).manifest;
  auto second = COVA_REQUIRE_OK(cova::manifest::load_from_root(second_root)).manifest;

  cova::build::WorkspaceProject project;
  project.members = {first, second};
  project.roots = {"first"};
  cova::build::PlanOptions options;
  options.target = cova::platform::host_triple();
  auto error = COVA_REQUIRE_ERR(cova::build::plan_workspace(
      project, dummy_toolchain("/usr/bin/ar"), options,
      std::filesystem::path(temp.path()) / "target"));
  COVA_CHECK_EQ(error.code, std::string("cova::build::dependency_cycle"));
}

COVA_TEST_NAMED(build, planner_builds_dependencies_for_the_requested_target) {
  cova::test::TempDir temp;
  auto fixture = make_path_dependency_fixture(temp.path());
  cova::build::WorkspaceProject project;
  project.members = {fixture.base, fixture.math, fixture.app};
  project.roots = {"app"};

  // Pick a target that is not the host: a cross build is what emits the
  // `--target` flag this test asserts on, and on an aarch64 host the usual
  // fixture target would be native.
  const bool host_is_aarch64 =
      cova::platform::host_triple().arch == cova::platform::Arch::Aarch64;
  const std::string target_text =
      host_is_aarch64 ? "x86_64-unknown-linux-gnu" : "aarch64-unknown-linux-gnu";
  const std::string target_short = host_is_aarch64 ? "x86_64" : "aarch64";
  auto target = COVA_REQUIRE_OK(cova::platform::Triple::parse(target_text));
  cova::build::PlanOptions options;
  options.target = target;
  options.profile = "dev";
  auto toolchain = dummy_toolchain("/usr/bin/" + target_short + "-linux-gnu-ar");
  toolchain.descriptor.family = "clang";
  auto plan = COVA_REQUIRE_OK(cova::build::plan_workspace(
      project, toolchain, options, std::filesystem::path(temp.path()) / "target"));

  bool saw_base_compile = false;
  for (const auto* action : plan.graph.actions_in_order()) {
    if (action->kind != cova::build::ActionKind::Compile) {
      continue;
    }
    // Every package, including transitive dependencies, targets the request.
    COVA_CHECK_MSG(action->argv.size() > 2, action->label);
    bool has_target_flag = false;
    for (const auto& argument : action->argv) {
      has_target_flag = has_target_flag || argument == "--target=" + target_text;
    }
    COVA_CHECK_MSG(has_target_flag, action->label);
    COVA_CHECK_MSG(action->outputs.front().path.string().find(target_short) != std::string::npos,
                   action->outputs.front().path.string());
    if (action->package == "base") {
      saw_base_compile = true;
    }
  }
  COVA_CHECK(saw_base_compile);
}

COVA_TEST_NAMED(build, planner_links_windows_print_through_libstdcxxexp) {
  cova::test::TempDir temp;
  auto fixture = make_path_dependency_fixture(temp.path());
  cova::build::WorkspaceProject project;
  project.members = {fixture.base, fixture.math, fixture.app};
  project.roots = {"app"};

  cova::build::PlanOptions options;
  options.target = COVA_REQUIRE_OK(cova::platform::Triple::parse("x86_64-pc-windows-gnu"));
  options.profile = "dev";
  auto plan = COVA_REQUIRE_OK(cova::build::plan_workspace(
      project, dummy_toolchain("/usr/bin/x86_64-w64-mingw32-ar"), options,
      std::filesystem::path(temp.path()) / "target"));

  bool saw_app_link = false;
  for (const auto* action : plan.graph.actions_in_order()) {
    if (action->kind != cova::build::ActionKind::Link || action->package != "app") {
      continue;
    }
    saw_app_link = true;
    const auto exp = std::find(action->argv.begin(), action->argv.end(), "-lstdc++exp");
    COVA_CHECK_MSG(exp != action->argv.end(), action->label);
    // GCC 16 emits the C++26 exception constructors in every object, and the
    // static libstdc++ defines them too; the two cannot share a PE image.
    const auto multiple =
        std::find(action->argv.begin(), action->argv.end(), "-Wl,--allow-multiple-definition");
    COVA_CHECK_MSG(multiple != action->argv.end(), action->label);
    // A static archive only resolves references from objects that precede it.
    const auto object = std::find_if(action->argv.begin(), action->argv.end(),
                                     [](const std::string& argument) {
                                       return argument.find("main.obj") != std::string::npos ||
                                              argument.find("main.o") != std::string::npos;
                                     });
    COVA_CHECK_MSG(object != action->argv.end() && object < exp, action->label);
  }
  COVA_CHECK(saw_app_link);
}

COVA_TEST_NAMED(build, planner_keeps_duplicate_definition_checks_for_clang) {
  cova::test::TempDir temp;
  auto fixture = make_path_dependency_fixture(temp.path());
  cova::build::WorkspaceProject project;
  project.members = {fixture.base, fixture.math, fixture.app};
  project.roots = {"app"};

  auto toolchain = dummy_toolchain("/usr/bin/x86_64-w64-mingw32-ar");
  toolchain.descriptor.family = "clang";
  cova::build::PlanOptions options;
  options.target = COVA_REQUIRE_OK(cova::platform::Triple::parse("x86_64-pc-windows-gnu"));
  options.profile = "dev";
  auto plan = COVA_REQUIRE_OK(cova::build::plan_workspace(
      project, toolchain, options, std::filesystem::path(temp.path()) / "target"));

  for (const auto* action : plan.graph.actions_in_order()) {
    if (action->kind != cova::build::ActionKind::Link) {
      continue;
    }
    const auto multiple =
        std::find(action->argv.begin(), action->argv.end(), "-Wl,--allow-multiple-definition");
    COVA_CHECK_MSG(multiple == action->argv.end(), action->label);
  }
}

COVA_TEST_NAMED(build, action_identity_tracks_environment_policy) {
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.argv = {"cc", "-c", "unit.cpp"};
  auto environment = host_environment();
  const std::string baseline = action.cache_key(environment);

  environment.environment["CPATH"] = "/opt/one";
  const std::string one = action.cache_key(environment);
  COVA_CHECK(one != baseline);
  environment.environment["CPATH"] = "/opt/two";
  const std::string two = action.cache_key(environment);
  COVA_CHECK(two != one);

  auto tracked = environment;
  tracked.isolation = cova::proc::EnvironmentIsolation::TrackedEnvironment;
  auto untracked = environment;
  untracked.isolation = cova::proc::EnvironmentIsolation::UntrackedEnvironment;
  COVA_CHECK(action.cache_key(tracked) != action.cache_key(untracked));
}

COVA_TEST_NAMED(build, action_identity_tracks_response_file_contents) {
  cova::test::TempDir temp;
  const std::filesystem::path response = std::filesystem::path(temp.path()) / "args.rsp";
  COVA_CHECK(cova::fs::write_file(response, "-O2\n").has_value());

  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.working_dir = std::filesystem::path(temp.path());
  action.argv = {"cc", "@" + response.string(), "-c", "unit.cpp"};
  auto environment = host_environment();
  const std::string first = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(response, "-O3\n").has_value());
  COVA_CHECK(first != action.cache_key(environment));

  const std::filesystem::path nested = std::filesystem::path(temp.path()) / "nested.rsp";
  COVA_CHECK(cova::fs::write_file(nested, "-O2\n").has_value());
  COVA_CHECK(cova::fs::write_file(response, "@" + nested.string() + "\n").has_value());
  const std::string outer = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(nested, "-O1\n").has_value());
  COVA_CHECK(outer != action.cache_key(environment));
}

COVA_TEST_NAMED(build, action_identity_tracks_include_directory_listings) {
  cova::test::TempDir temp;
  const std::filesystem::path include = std::filesystem::path(temp.path()) / "include";
  COVA_CHECK(cova::fs::create_directories(include).has_value());
  COVA_CHECK(cova::fs::write_file(include / "a.hpp", "#pragma once\n").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.argv = {"cc", "-I" + include.string(), "-c", "unit.cpp"};
  auto environment = host_environment();
  const std::string first = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(include / "b.hpp", "#pragma once\n").has_value());
  // A new higher-priority header can change `#include <b.hpp>` resolution even
  // when no depfile has been regenerated yet.
  COVA_CHECK(first != action.cache_key(environment));
}

COVA_TEST_NAMED(build, action_identity_tracks_attached_system_include_dirs) {
  cova::test::TempDir temp;
  const std::filesystem::path include = std::filesystem::path(temp.path()) / "include";
  COVA_CHECK(cova::fs::create_directories(include).has_value());
  COVA_CHECK(cova::fs::write_file(include / "a.hpp", "#pragma once\n").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.argv = {"cc", "-isystem" + include.string(), "-c", "unit.cpp"};
  auto environment = host_environment();
  const std::string first = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(include / "b.hpp", "#pragma once\n").has_value());
  COVA_CHECK(first != action.cache_key(environment));
}

COVA_TEST_NAMED(build, artifact_cache_restores_and_detects_conflicts) {
  cova::test::TempDir temp;
  cova::build::CacheSettings settings;
  settings.root = std::filesystem::path(temp.path()) / "cache";
  const std::filesystem::path output = std::filesystem::path(temp.path()) / "out.o";
  COVA_CHECK(cova::fs::write_file(output, "first-content").has_value());

  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.outputs.push_back(cova::build::FileOutput{output, true});
  COVA_CHECK(cova::build::cache_store(settings, action, "abc").has_value());
  auto unsafe_store = cova::build::cache_store(settings, action, "../escape");
  COVA_CHECK(!unsafe_store.has_value());
  COVA_CHECK_EQ(unsafe_store.error().code, std::string("cova::build::cache_key"));

  COVA_CHECK(cova::fs::write_file(output, "garbage").has_value());
  const bool restored = COVA_REQUIRE_OK(cova::build::cache_restore(settings, action, "abc"));
  COVA_CHECK(restored);
  COVA_CHECK_EQ(COVA_REQUIRE_OK(cova::fs::read_file(output)), std::string("first-content"));

  COVA_CHECK(cova::fs::write_file(output, "different-content").has_value());
  auto conflict = cova::build::cache_store(settings, action, "abc");
  COVA_CHECK(!conflict.has_value());
  COVA_CHECK_EQ(conflict.error().code, std::string("cova::build::cache_nondeterminism"));

  // Reuse is suspended while the non-determinism incident is unresolved.
  COVA_CHECK(cova::fs::write_file(output, "garbage").has_value());
  const bool suspended = COVA_REQUIRE_OK(cova::build::cache_restore(settings, action, "abc"));
  COVA_CHECK(!suspended);

  // The original entry was not overwritten; explaining/clearing the incident
  // makes it restorable again.
  const auto entry = settings.root / "actions" / "ab" / "abc";
  COVA_CHECK(cova::fs::remove_file(entry / "nondeterminism.toml").has_value());
  const bool existing = COVA_REQUIRE_OK(cova::build::cache_restore(settings, action, "abc"));
  COVA_CHECK(existing);
  COVA_CHECK_EQ(COVA_REQUIRE_OK(cova::fs::read_file(output)), std::string("first-content"));
}

COVA_TEST_NAMED(build, artifact_cache_rejects_corruption_and_schema_mismatch) {
  cova::test::TempDir temp;
  cova::build::CacheSettings settings;
  settings.root = std::filesystem::path(temp.path()) / "cache";
  const std::filesystem::path output = std::filesystem::path(temp.path()) / "out.o";
  COVA_CHECK(cova::fs::write_file(output, "payload").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.outputs.push_back(cova::build::FileOutput{output, true});
  COVA_CHECK(cova::build::cache_store(settings, action, "abc").has_value());

  const auto entry = settings.root / "actions" / "ab" / "abc";
  std::filesystem::path blob;
  for (const auto& item : std::filesystem::directory_iterator(entry)) {
    const std::string name = item.path().filename().string();
    if (name.rfind("blob-", 0) == 0) {
      blob = item.path();
    }
  }
  COVA_CHECK(!blob.empty());
  COVA_CHECK(cova::fs::write_file(blob, "corrupt").has_value());
  const bool corrupt = COVA_REQUIRE_OK(cova::build::cache_restore(settings, action, "abc"));
  COVA_CHECK(!corrupt);

  COVA_CHECK(cova::build::cache_store(settings, action, "def").has_value());
  const auto manifest = settings.root / "actions" / "de" / "def" / "manifest.toml";
  auto text = COVA_REQUIRE_OK(cova::fs::read_file(manifest));
  text = cova::str::replace_all(text, std::string(cova::build::kArtifactCacheSchemaVersion),
                                "cova.artifact-cache/1");
  COVA_CHECK(cova::fs::write_file(manifest, text).has_value());
  const bool mismatch = COVA_REQUIRE_OK(cova::build::cache_restore(settings, action, "def"));
  COVA_CHECK(!mismatch);
}

COVA_TEST_NAMED(build, build_state_schema_mismatch_is_ignored) {
  cova::test::TempDir temp;
  const auto path = std::filesystem::path(temp.path()) / "state.toml";
  COVA_CHECK(cova::fs::write_file(path, R"(
schema-version = "cova.build-state/1"

[[record]]
cache-key = "old"
succeeded = true
)")
                 .has_value());
  auto state = COVA_REQUIRE_OK(cova::build::BuildState::load(path));
  COVA_CHECK_EQ(state.size(), std::size_t{0});
}

COVA_TEST_NAMED(build, action_identity_resolves_nested_response_files_against_cwd) {
  cova::test::TempDir temp;
  const std::filesystem::path root(temp.path());
  COVA_CHECK(cova::fs::create_directories(root / "rsp").has_value());
  COVA_CHECK(cova::fs::write_file(root / "rsp" / "main.rsp", "@nested.rsp\n").has_value());
  COVA_CHECK(cova::fs::write_file(root / "nested.rsp", "-DVALUE=1\n").has_value());
  COVA_CHECK(cova::fs::write_file(root / "rsp" / "nested.rsp", "-DVALUE=2\n").has_value());

  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.working_dir = root;
  action.argv = {"cc", "@rsp/main.rsp", "-c", "unit.cpp"};
  auto environment = host_environment();
  const std::string first = action.cache_key(environment);

  // GCC and Clang resolve nested @-files relative to the driver's cwd, not the
  // containing response file. The unused `rsp/nested.rsp` must not affect the
  // identity; the cwd file must.
  COVA_CHECK(cova::fs::write_file(root / "rsp" / "nested.rsp", "-DVALUE=20\n").has_value());
  COVA_CHECK_EQ(first, action.cache_key(environment));
  COVA_CHECK(cova::fs::write_file(root / "nested.rsp", "-DVALUE=10\n").has_value());
  COVA_CHECK(first != action.cache_key(environment));

  // Multi-level nesting is followed as well.
  COVA_CHECK(cova::fs::write_file(root / "rsp" / "main.rsp", "@mid.rsp\n").has_value());
  COVA_CHECK(cova::fs::write_file(root / "mid.rsp", "@deep.rsp\n").has_value());
  COVA_CHECK(cova::fs::write_file(root / "deep.rsp", "-DVALUE=3\n").has_value());
  const std::string multi = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(root / "deep.rsp", "-DVALUE=4\n").has_value());
  COVA_CHECK(multi != action.cache_key(environment));

  // Backslash-escaped spaces are part of GCC's response-file syntax.
  COVA_CHECK(cova::fs::write_file(root / "rsp" / "main.rsp", "@spaced\\ file.rsp\n")
                 .has_value());
  COVA_CHECK(cova::fs::write_file(root / "spaced file.rsp", "-DVALUE=5\n").has_value());
  const std::string spaced = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(root / "spaced file.rsp", "-DVALUE=6\n").has_value());
  COVA_CHECK(spaced != action.cache_key(environment));
}

COVA_TEST_NAMED(build, action_identity_tracks_linker_response_files) {
  cova::test::TempDir temp;
  const std::filesystem::path root(temp.path());
  COVA_CHECK(cova::fs::write_file(root / "link.rsp", "@nested.rsp\n").has_value());
  COVA_CHECK(cova::fs::write_file(root / "nested.rsp", "--as-needed\n").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Link;
  action.working_dir = root;
  action.argv = {"cc", "-Wl,@link.rsp", "-o", "program"};
  auto environment = host_environment();
  const std::string first = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(root / "nested.rsp", "--no-as-needed\n").has_value());
  COVA_CHECK(first != action.cache_key(environment));
}

COVA_TEST_NAMED(build, action_identity_detects_response_file_cycles) {
  cova::test::TempDir temp;
  const std::filesystem::path root(temp.path());
  COVA_CHECK(cova::fs::write_file(root / "a.rsp", "@b.rsp\n").has_value());
  COVA_CHECK(cova::fs::write_file(root / "b.rsp", "@a.rsp\n").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.working_dir = root;
  action.argv = {"cc", "@a.rsp", "-c", "unit.cpp"};
  auto environment = host_environment();
  const std::string first = action.cache_key(environment);
  const std::string second = action.cache_key(environment);
  COVA_CHECK_EQ(first, second);
  COVA_CHECK(!action.input_identity_complete);
}

COVA_TEST_NAMED(build, action_identity_tracks_xclang_plugin_loads) {
  cova::test::TempDir temp;
  const std::filesystem::path root(temp.path());
  const auto plugin = root / "plugin.so";
  COVA_CHECK(cova::fs::write_file(plugin, "plugin-one").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.working_dir = root;
  action.argv = {"clang", "-Xclang", "-load", "-Xclang", plugin.string(), "-c", "unit.cpp"};
  auto environment = host_environment();
  const std::string first = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(plugin, "plugin-two").has_value());
  COVA_CHECK(first != action.cache_key(environment));
}

COVA_TEST_NAMED(build, action_identity_tracks_plugin_and_specs_files) {
  cova::test::TempDir temp;
  const std::filesystem::path root(temp.path());
  const auto plugin = root / "plugin.so";
  const auto specs = root / "cova.specs";
  COVA_CHECK(cova::fs::write_file(plugin, "plugin-one").has_value());
  COVA_CHECK(cova::fs::write_file(specs, "specs-one").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.working_dir = root;
  action.argv = {"cc", "-fplugin=" + plugin.string(), "-specs=" + specs.string(), "-c",
                 "unit.cpp"};
  auto environment = host_environment();
  const std::string first = action.cache_key(environment);
  COVA_CHECK(cova::fs::write_file(plugin, "plugin-two").has_value());
  COVA_CHECK(first != action.cache_key(environment));
  COVA_CHECK(cova::fs::write_file(specs, "specs-two").has_value());
  COVA_CHECK(first != action.cache_key(environment));
}

#ifndef _WIN32
COVA_TEST_NAMED(build, unreadable_include_directory_makes_identity_incomplete) {
  if (::geteuid() == 0) {
    return;  // root can read a 0000 directory, so the case is not reproducible.
  }
  cova::test::TempDir temp;
  const std::filesystem::path include = std::filesystem::path(temp.path()) / "include";
  COVA_CHECK(cova::fs::create_directories(include).has_value());
  COVA_CHECK(cova::fs::write_file(include / "a.hpp", "#pragma once\n").has_value());
  COVA_CHECK(::chmod(include.c_str(), 0) == 0);
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.argv = {"cc", "-I" + include.string(), "-c", "unit.cpp"};
  action.cache_key(host_environment());
  const bool incomplete = !action.input_identity_complete;
  COVA_CHECK(::chmod(include.c_str(), 0700) == 0);
  COVA_CHECK(incomplete);
}
#endif

COVA_TEST_NAMED(build, state_inputs_match_is_independent_of_output_presence) {
  cova::test::TempDir temp;
  const auto input = std::filesystem::path(temp.path()) / "input.cpp";
  const auto output = std::filesystem::path(temp.path()) / "out.o";
  COVA_CHECK(cova::fs::write_file(input, "int a = 1;\n").has_value());
  cova::build::Action action;
  action.kind = cova::build::ActionKind::Compile;
  action.argv = {"cc", "-c", cova::fs::generic_string(input), "-o",
                 cova::fs::generic_string(output)};
  action.inputs.push_back(cova::build::FileInput{input, "source"});
  action.outputs.push_back(cova::build::FileOutput{output, true});
  auto environment = host_environment();
  const std::string key = action.cache_key(environment);

  COVA_CHECK(cova::fs::write_file(output, "object\n").has_value());
  cova::build::ActionRecord record;
  record.cache_key = key;
  record.output_paths.push_back(cova::fs::generic_string(output));
  record.output_hashes.emplace_back(
      cova::fs::generic_string(output),
      cova::to_hex(*cova::build::FileHasher{}.hash_file(output)));
  record.input_hashes.emplace_back(
      cova::fs::generic_string(input),
      cova::to_hex(*cova::build::FileHasher{}.hash_file(input)));
  record.succeeded = true;
  cova::build::BuildState state;
  state.record(std::move(record));

  COVA_CHECK(state.inputs_match(action));
  COVA_CHECK(cova::fs::remove_file(output).has_value());
  std::string reason;
  COVA_CHECK(!state.is_up_to_date(action, reason));
  COVA_CHECK(cova::fs::write_file(output, "object\n").has_value());
  COVA_CHECK(state.is_up_to_date(action, reason));
}

COVA_TEST_MAIN()
