// Cova — local dependency resolution and lockfile enforcement (PRD §10, §15).
#include <algorithm>
#include <string>
#include <string_view>

#include "core/filesystem/filesystem.hpp"
#include "manifest/manifest.hpp"
#include "package/dependency/dependency.hpp"
#include "package/lockfile/lockfile.hpp"
#include "platform/triple/triple.hpp"
#include "testing/test.hpp"

namespace {

namespace stdfs = std::filesystem;

std::string package_manifest(std::string_view name, std::string_view kind,
                             std::string_view dependencies = {}) {
  return "[package]\nname = \"" + std::string(name) +
         "\"\nversion = \"0.1.0\"\n[build]\ntype = \"" + std::string(kind) +
         "\"\nsources = [\"src/*.cpp\"]\n" + std::string(dependencies);
}

void write_project(const stdfs::path& root, std::string_view name, std::string_view kind,
                   std::string_view dependencies = {}) {
  COVA_CHECK(cova::fs::create_directories(root / "src").has_value());
  COVA_CHECK(cova::fs::write_file(root / "cova.toml",
                                  package_manifest(name, kind, dependencies))
                 .has_value());
  COVA_CHECK(cova::fs::write_file(root / "src" / "source.cpp", "int value() { return 0; }\n")
                 .has_value());
}

cova::manifest::Manifest load_manifest(const stdfs::path& root) {
  auto loaded = cova::manifest::load_from_root(root);
  COVA_CHECK_MSG(loaded.has_value(), loaded ? "" : loaded.error().to_string());
  return loaded->manifest;
}

cova::package::ResolveRequest request_for(std::vector<cova::manifest::Manifest> roots,
                                         const stdfs::path& lock_root) {
  cova::package::ResolveRequest request;
  request.target = cova::platform::host_triple();
  request.roots = std::move(roots);
  request.lock_root = lock_root;
  return request;
}

}  // namespace

COVA_TEST_NAMED(package, resolves_transitive_path_dependencies) {
  cova::test::TempDir temp;
  const auto base_root = stdfs::path(temp.path()) / "base";
  const auto math_root = stdfs::path(temp.path()) / "math";
  const auto app_root = stdfs::path(temp.path()) / "app";
  write_project(base_root, "base", "static-library");
  write_project(math_root, "math", "static-library",
                "[dependencies]\nbase = { path = \"../base\" }\n");
  write_project(app_root, "app", "executable",
                "[dependencies]\nmath = { path = \"../math\" }\n");

  auto graph = COVA_REQUIRE_OK(
      cova::package::resolve(request_for({load_manifest(app_root)}, app_root)));
  COVA_CHECK_EQ(graph.packages.size(), std::size_t{3});
  // Dependency-first order.
  COVA_CHECK_EQ(graph.packages[0].manifest.package.name, std::string("base"));
  COVA_CHECK_EQ(graph.packages[1].manifest.package.name, std::string("math"));
  COVA_CHECK_EQ(graph.packages[2].manifest.package.name, std::string("app"));

  const auto* app = graph.find("app");
  const auto* math = graph.find("math");
  const auto* base = graph.find("base");
  COVA_CHECK(app != nullptr);
  COVA_CHECK(math != nullptr);
  COVA_CHECK(base != nullptr);
  COVA_CHECK(app->root);
  COVA_CHECK(!math->root);
  COVA_CHECK(app->build_dependencies == std::vector<std::string>{"math"});
  COVA_CHECK(math->build_dependencies == std::vector<std::string>{"base"});
  COVA_CHECK(!graph.fingerprint.empty());

  // Fingerprints are stable across repeated resolutions.
  auto again =
      COVA_REQUIRE_OK(cova::package::resolve(request_for({load_manifest(app_root)}, app_root)));
  COVA_CHECK_EQ(again.fingerprint, graph.fingerprint);
}

COVA_TEST_NAMED(package, rejects_dependency_cycles) {
  cova::test::TempDir temp;
  const auto a_root = stdfs::path(temp.path()) / "a";
  const auto b_root = stdfs::path(temp.path()) / "b";
  write_project(a_root, "a", "static-library",
                "[dependencies]\nb = { path = \"../b\" }\n");
  write_project(b_root, "b", "static-library",
                "[dependencies]\na = { path = \"../a\" }\n");

  auto error =
      COVA_REQUIRE_ERR(cova::package::resolve(request_for({load_manifest(a_root)}, a_root)));
  COVA_CHECK_EQ(error.code, std::string("cova::package::cycle"));
  COVA_CHECK_MSG(error.message.find("a -> b -> a") != std::string::npos, error.message);
}

COVA_TEST_NAMED(package, rejects_remote_dependency_declarations) {
  cova::test::TempDir temp;
  const auto app_root = stdfs::path(temp.path()) / "app";
  write_project(app_root, "app", "executable", "[dependencies]\nfmt = \"11\"\n");

  auto error =
      COVA_REQUIRE_ERR(cova::package::resolve(request_for({load_manifest(app_root)}, app_root)));
  COVA_CHECK_EQ(error.code, std::string("cova::package::not_implemented"));
  COVA_CHECK_MSG(error.message.find("fmt") != std::string::npos, error.message);
  bool mentions_remote = false;
  for (const auto& line : error.context) {
    mentions_remote = mentions_remote || line.find("remote dependency management") != std::string::npos;
  }
  COVA_CHECK(mentions_remote);
}

COVA_TEST_NAMED(package, target_filters_change_the_build_edges_not_the_lock) {
  cova::test::TempDir temp;
  const auto math_root = stdfs::path(temp.path()) / "math";
  const auto app_root = stdfs::path(temp.path()) / "app";
  // A target that is not this host, so the filter really excludes the edge on
  // both x86_64 and aarch64 runners.
  const std::string other_target =
      cova::platform::host_triple().arch == cova::platform::Arch::Aarch64
          ? "x86_64-unknown-linux-gnu"
          : "aarch64-unknown-linux-gnu";
  write_project(math_root, "math", "static-library");
  write_project(app_root, "app", "executable",
                "[dependencies]\nmath = { path = \"../math\", target = [\"" + other_target +
                    "\"] }\n");

  auto graph = COVA_REQUIRE_OK(
      cova::package::resolve(request_for({load_manifest(app_root)}, app_root)));
  const auto* app = graph.find("app");
  COVA_CHECK(app != nullptr);
  // Not built for this host target...
  COVA_CHECK(app->build_dependencies.empty());
  // ...but still recorded, so the shared lockfile is target-stable.
  COVA_CHECK(app->all_dependencies == std::vector<std::string>{"math"});
}

COVA_TEST_NAMED(package, rejects_packages_with_the_same_name_from_different_paths) {
  cova::test::TempDir temp;
  const auto workspace_root = stdfs::path(temp.path()) / "ws";
  const auto member_root = workspace_root / "math";
  const auto other_root = workspace_root / "vendor" / "math";
  const auto app_root = workspace_root / "app";
  write_project(member_root, "math", "static-library");
  write_project(other_root, "math", "static-library");
  write_project(app_root, "app", "executable",
                "[dependencies]\nmath = { path = \"../vendor/math\" }\n");

  cova::package::ResolveRequest request;
  request.target = cova::platform::host_triple();
  request.workspace_members = {load_manifest(member_root)};
  request.roots = {load_manifest(app_root)};
  request.lock_root = workspace_root;

  auto error = COVA_REQUIRE_ERR(cova::package::resolve(request));
  COVA_CHECK_EQ(error.code, std::string("cova::package::duplicate"));
  bool mentions_both_paths = false;
  for (const auto& line : error.context) {
    if (line.find("math") != std::string::npos) {
      mentions_both_paths = true;
    }
  }
  COVA_CHECK(mentions_both_paths);
}

COVA_TEST_NAMED(package, rejects_a_dependency_key_that_disagrees_with_the_package_name) {
  cova::test::TempDir temp;
  const auto math_root = stdfs::path(temp.path()) / "math";
  const auto app_root = stdfs::path(temp.path()) / "app";
  write_project(math_root, "math", "static-library");
  write_project(app_root, "app", "executable",
                "[dependencies]\nnumbers = { path = \"../math\" }\n");

  auto error =
      COVA_REQUIRE_ERR(cova::package::resolve(request_for({load_manifest(app_root)}, app_root)));
  COVA_CHECK_EQ(error.code, std::string("cova::package::name_mismatch"));
}

COVA_TEST_NAMED(package, resolves_name_only_dependencies_to_workspace_members) {
  cova::test::TempDir temp;
  const auto workspace_root = stdfs::path(temp.path()) / "ws";
  const auto math_root = workspace_root / "math";
  const auto app_root = workspace_root / "app";
  write_project(math_root, "math", "static-library");
  write_project(app_root, "app", "executable", "[dependencies]\nmath = \"0.1.0\"\n");

  cova::package::ResolveRequest request;
  request.target = cova::platform::host_triple();
  request.workspace_members = {load_manifest(math_root)};
  request.roots = {load_manifest(app_root)};
  request.lock_root = workspace_root;

  auto graph = COVA_REQUIRE_OK(cova::package::resolve(request));
  const auto* app = graph.find("app");
  COVA_CHECK(app != nullptr);
  COVA_CHECK(app->build_dependencies == std::vector<std::string>{"math"});
  const auto* math = graph.find("math");
  COVA_CHECK(math != nullptr);
  COVA_CHECK(math->manifest.path.string().find("cova.toml") != std::string::npos);
}

COVA_TEST_NAMED(package, development_dependencies_are_locked_but_not_built_by_default) {
  cova::test::TempDir temp;
  const auto math_root = stdfs::path(temp.path()) / "math";
  const auto app_root = stdfs::path(temp.path()) / "app";
  write_project(math_root, "math", "static-library");
  write_project(app_root, "app", "executable",
                "[dev-dependencies]\nmath = { path = \"../math\" }\n");

  auto build_request = request_for({load_manifest(app_root)}, app_root);
  build_request.include_dev = false;
  auto build_graph = COVA_REQUIRE_OK(cova::package::resolve(build_request));
  const auto* app = build_graph.find("app");
  COVA_CHECK(app != nullptr);
  // Locked, but not part of a regular build.
  COVA_CHECK(app->all_dependencies == std::vector<std::string>{"math"});
  COVA_CHECK(app->build_dependencies.empty());
  COVA_CHECK(build_graph.find("math") != nullptr);

  auto test_request = request_for({load_manifest(app_root)}, app_root);
  test_request.include_dev = true;
  auto test_graph = COVA_REQUIRE_OK(cova::package::resolve(test_request));
  const auto* test_app = test_graph.find("app");
  COVA_CHECK(test_app != nullptr);
  COVA_CHECK(test_app->build_dependencies == std::vector<std::string>{"math"});
}

COVA_TEST_NAMED(package, skips_remote_development_dependencies_for_a_regular_build) {
  cova::test::TempDir temp;
  const auto app_root = stdfs::path(temp.path()) / "app";
  write_project(app_root, "app", "executable", "[dev-dependencies]\ncatch2 = \"3\"\n");

  auto build_request = request_for({load_manifest(app_root)}, app_root);
  build_request.include_dev = false;
  auto graph = COVA_REQUIRE_OK(cova::package::resolve(build_request));
  COVA_CHECK_EQ(graph.packages.size(), std::size_t{1});

  // A test build does need it, and reports the missing capability honestly.
  auto test_request = request_for({load_manifest(app_root)}, app_root);
  test_request.include_dev = true;
  auto error = COVA_REQUIRE_ERR(cova::package::resolve(test_request));
  COVA_CHECK_EQ(error.code, std::string("cova::package::not_implemented"));
}

COVA_TEST_NAMED(package, lockfile_is_written_enforced_and_never_rewritten_in_locked_mode) {
  cova::test::TempDir temp;
  const auto math_root = stdfs::path(temp.path()) / "math";
  const auto app_root = stdfs::path(temp.path()) / "app";
  write_project(math_root, "math", "static-library");
  write_project(app_root, "app", "executable",
                "[dependencies]\nmath = { path = \"../math\" }\n");
  const auto lock_path = app_root / "cova.lock";

  auto graph = COVA_REQUIRE_OK(
      cova::package::resolve(request_for({load_manifest(app_root)}, app_root)));
  auto first = COVA_REQUIRE_OK(cova::package::enforce_lock(graph, lock_path, false));
  COVA_CHECK(first.wrote);
  COVA_CHECK(cova::fs::is_regular_file(lock_path));

  auto second = COVA_REQUIRE_OK(cova::package::enforce_lock(graph, lock_path, false));
  COVA_CHECK(!second.wrote);
  COVA_CHECK(second.up_to_date);

  auto locked = COVA_REQUIRE_OK(cova::package::enforce_lock(graph, lock_path, true));
  COVA_CHECK(locked.up_to_date);
  COVA_CHECK(!locked.wrote);

  auto recorded = COVA_REQUIRE_OK(cova::lockfile::DependencyLock::load(lock_path));
  auto* math = recorded.find("math");
  COVA_CHECK(math != nullptr);
  COVA_CHECK(math->path.has_value());
  COVA_CHECK_EQ(*math->path, std::string("../math"));

  // A changed dependency declaration makes the lock stale.
  COVA_CHECK(cova::fs::write_file(
                 app_root / "cova.toml",
                 package_manifest("app", "executable",
                                  "[dependencies]\nmath = { path = \"../math\", version = "
                                  "\"0.2.0\" }\n"))
                 .has_value());
  auto changed = COVA_REQUIRE_OK(
      cova::package::resolve(request_for({load_manifest(app_root)}, app_root)));
  const std::string before = *cova::fs::read_file(lock_path);
  auto stale = COVA_REQUIRE_ERR(cova::package::enforce_lock(changed, lock_path, true));
  COVA_CHECK_EQ(stale.code, std::string("cova::lock::stale"));
  const std::string after = *cova::fs::read_file(lock_path);
  COVA_CHECK_EQ(after, before);

  // Missing lockfiles are an error in locked mode and are created otherwise.
  const auto missing = app_root / "missing.lock";
  auto absent = COVA_REQUIRE_ERR(cova::package::enforce_lock(changed, missing, true));
  COVA_CHECK_EQ(absent.code, std::string("cova::lock::missing"));
  auto created = COVA_REQUIRE_OK(cova::package::enforce_lock(changed, missing, false));
  COVA_CHECK(created.wrote);
  COVA_CHECK(cova::fs::is_regular_file(missing));
}

COVA_TEST_MAIN()
