// Cova — build graph, cache, depfile and planner tests (PRD §9).
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

COVA_TEST_MAIN()
