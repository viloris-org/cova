// Cova — build graph, cache, depfile and planner tests (PRD §9).
#include <algorithm>
#include <string>

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

COVA_TEST_MAIN()
