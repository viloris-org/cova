// Cova — local dependency resolution and lockfile enforcement (PRD §10, §15).
#include <algorithm>
#include <string>
#include <string_view>

#include "core/filesystem/filesystem.hpp"
#include "core/process/process.hpp"
#include "manifest/manifest.hpp"
#include "package/dependency/dependency.hpp"
#include "package/lockfile/lockfile.hpp"
#include "package/source/git.hpp"
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

bool have_git() { return cova::fs::find_in_path("git").has_value(); }

// Runs git in `cwd`; a non-zero exit is a test failure, not an expected state.
void git_run(const stdfs::path& cwd, const std::vector<std::string>& args) {
  auto git = cova::fs::find_in_path("git");
  COVA_CHECK(git.has_value());
  auto result = cova::proc::run(*git, args, cwd);
  COVA_CHECK_MSG(result.has_value(), result ? "" : result.error().to_string());
  COVA_CHECK_MSG(result->success(), "git failed: " + result->stderr_trimmed());
}

// Runs git in `cwd` and returns its trimmed stdout.
std::string git_output(const stdfs::path& cwd, const std::vector<std::string>& args) {
  auto git = cova::fs::find_in_path("git");
  COVA_CHECK(git.has_value());
  auto result = cova::proc::capture(*git, args, cwd);
  COVA_CHECK_MSG(result.has_value(), result ? "" : result.error().to_string());
  return cova::str::trim(*result);
}

// Creates a committed library repository at `root`. The branch is pinned to
// `master` on purpose: `git init` otherwise follows the developer's
// `init.defaultBranch`, and tests that name a branch would fail on a machine
// configured for `main`.
void init_library_repo(const stdfs::path& root, std::string_view name) {
  write_project(root, name, "static-library");
  git_run(root, {"init", "--quiet"});
  git_run(root, {"symbolic-ref", "HEAD", "refs/heads/master"});
  git_run(root, {"-c", "user.email=test@cova.invalid", "-c", "user.name=Cova Test", "add",
                 "-A"});
  git_run(root, {"-c", "user.email=test@cova.invalid", "-c", "user.name=Cova Test", "commit",
                 "--quiet", "-m", "initial"});
}

void commit_all(const stdfs::path& root, std::string_view message) {
  git_run(root, {"-c", "user.email=test@cova.invalid", "-c", "user.name=Cova Test", "add",
                 "-A"});
  git_run(root, {"-c", "user.email=test@cova.invalid", "-c", "user.name=Cova Test", "commit",
                 "--quiet", "-m", std::string(message)});
}

// `rev` may be empty: that is the "use the repository default branch" form.
std::string git_dependency(std::string_view name, const stdfs::path& url,
                           std::string_view rev = {}) {
  std::string text = "[dependencies]\n" + std::string(name) + " = { git = \"" +
                     cova::fs::generic_string(url) + "\"";
  if (!rev.empty()) {
    text += ", rev = \"" + std::string(rev) + "\"";
  }
  return text + " }\n";
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

COVA_TEST_NAMED(package, rejects_registry_dependency_declarations) {
  cova::test::TempDir temp;
  const auto app_root = stdfs::path(temp.path()) / "app";
  write_project(app_root, "app", "executable", "[dependencies]\nfmt = \"11\"\n");

  auto error =
      COVA_REQUIRE_ERR(cova::package::resolve(request_for({load_manifest(app_root)}, app_root)));
  COVA_CHECK_EQ(error.code, std::string("cova::package::not_implemented"));
  COVA_CHECK_MSG(error.message.find("fmt") != std::string::npos, error.message);
  bool mentions_registry = false;
  for (const auto& line : error.context) {
    mentions_registry =
        mentions_registry || line.find("registry dependencies are not implemented") != std::string::npos;
  }
  COVA_CHECK(mentions_registry);
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

COVA_TEST_NAMED(package, resolves_git_dependency_and_pins_its_commit) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  const auto cache = base / "git-cache";
  init_library_repo(lib_root, "math");
  git_run(lib_root, {"tag", "v1"});
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "v1"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = cache;
  auto graph = COVA_REQUIRE_OK(cova::package::resolve(request));

  const auto* math = graph.find("math");
  COVA_CHECK(math != nullptr);
  COVA_CHECK(math->source_url.has_value());
  COVA_CHECK_EQ(*math->source_url, cova::fs::generic_string(lib_root));
  // The lockfile records a commit, never a tag: a moved tag cannot silently
  // change an existing lock.
  COVA_CHECK(math->git_rev.has_value());
  COVA_CHECK_EQ(math->git_rev->size(), std::size_t{40});
  COVA_CHECK_EQ(math->source_hash.size(), std::size_t{64});
  // Built from the cache checkout, not from the repository next to it.
  COVA_CHECK(cova::fs::is_within(math->manifest.root, cache));
  COVA_CHECK(cova::fs::is_regular_file(math->manifest.root / "cova.toml"));

  const auto* app = graph.find("app");
  COVA_CHECK(app != nullptr);
  COVA_CHECK(app->build_dependencies == std::vector<std::string>{"math"});

  // The lockfile identifies the source by URL, commit and tree hash, and must
  // not record a machine-specific cache path.
  auto lock = cova::package::to_lock(graph, app_root);
  const auto* entry = lock.find("math");
  COVA_CHECK(entry != nullptr);
  COVA_CHECK(entry->source_url.has_value());
  COVA_CHECK(!entry->path.has_value());
  COVA_CHECK_EQ(*entry->git_rev, *math->git_rev);
  COVA_CHECK_EQ(entry->source_hash, math->source_hash);
}

COVA_TEST_NAMED(package, keeps_a_pinned_commit_when_the_branch_moves) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  const auto cache = base / "git-cache";
  init_library_repo(lib_root, "math");
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "master"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = cache;
  auto first = COVA_REQUIRE_OK(cova::package::resolve(request));
  const std::string pinned = *first.find("math")->git_rev;
  // A real second build has the lockfile the first one wrote; feed it in, or the
  // resolution is a *first* resolution and is expected to read the remote.
  const auto lock = cova::package::to_lock(first, app_root);

  // A new commit on the branch must not change the resolution of a plain build.
  COVA_CHECK(cova::fs::write_file(lib_root / "src" / "source.cpp",
                                  "int value() { return 1; }\n")
                 .has_value());
  commit_all(lib_root, "second");

  request.existing_lock = &lock;
  auto stale = COVA_REQUIRE_OK(cova::package::resolve(request));
  COVA_CHECK_EQ(*stale.find("math")->git_rev, pinned);

  // `cova update` is the only command that re-reads remote refs.
  auto refresh = request;
  refresh.refresh_git = true;
  auto advanced = COVA_REQUIRE_OK(cova::package::resolve(refresh));
  COVA_CHECK_NE(*advanced.find("math")->git_rev, pinned);
}

COVA_TEST_NAMED(package, reuses_the_cached_checkout_without_network_access) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  const auto cache = base / "git-cache";
  init_library_repo(lib_root, "math");
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "master"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = cache;
  auto first = COVA_REQUIRE_OK(cova::package::resolve(request));

  // Move the repository away: an offline resolve must still answer from the
  // cache, which proves the checkout is self-contained.
  std::error_code ec;
  stdfs::rename(lib_root, base / "lib-moved", ec);
  COVA_CHECK(!ec);

  auto offline = request;
  offline.offline = true;
  auto cached = COVA_REQUIRE_OK(cova::package::resolve(offline));
  COVA_CHECK_EQ(*cached.find("math")->git_rev, *first.find("math")->git_rev);
  COVA_CHECK_EQ(cached.find("math")->source_hash, first.find("math")->source_hash);
}

COVA_TEST_NAMED(package, rejects_uncached_git_dependency_in_offline_mode) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  init_library_repo(lib_root, "math");
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "master"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = base / "empty-cache";
  request.offline = true;
  auto error = COVA_REQUIRE_ERR(cova::package::resolve(request));
  COVA_CHECK_EQ(error.code, std::string("cova::package::source_not_cached"));
}

COVA_TEST_NAMED(package, rejects_git_revisions_that_do_not_exist) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  init_library_repo(lib_root, "math");
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "no-such-tag"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = base / "git-cache";
  auto error = COVA_REQUIRE_ERR(cova::package::resolve(request));
  COVA_CHECK_EQ(error.code, std::string("cova::package::source_rev_unknown"));
}

COVA_TEST_NAMED(package, rejects_git_dependency_whose_package_name_differs) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  init_library_repo(lib_root, "math");
  write_project(app_root, "app", "executable", git_dependency("util", lib_root, "master"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = base / "git-cache";
  auto error = COVA_REQUIRE_ERR(cova::package::resolve(request));
  COVA_CHECK_EQ(error.code, std::string("cova::package::name_mismatch"));
}

COVA_TEST_NAMED(package, pins_the_locked_commit_over_a_moved_branch) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  const auto cache = base / "git-cache";
  init_library_repo(lib_root, "math");
  git_run(lib_root, {"tag", "v1"});
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "master"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = cache;
  auto first = COVA_REQUIRE_OK(cova::package::resolve(request));
  const std::string pinned = *first.find("math")->git_rev;
  const auto lock = cova::package::to_lock(first, app_root);

  COVA_CHECK(cova::fs::write_file(lib_root / "src" / "source.cpp",
                                  "int value() { return 2; }\n")
                 .has_value());
  commit_all(lib_root, "moved");
  git_run(lib_root, {"tag", "v2"});

  // A cold cache plus a lockfile must build the locked commit, not today's
  // branch head: this is what makes `--locked` reproducible on CI.
  auto replay = request_for({load_manifest(app_root)}, app_root);
  replay.git_cache = base / "cold-cache";
  replay.existing_lock = &lock;
  auto second = COVA_REQUIRE_OK(cova::package::resolve(replay));
  COVA_CHECK_EQ(*second.find("math")->git_rev, pinned);
  COVA_CHECK_EQ(second.find("math")->source_hash, first.find("math")->source_hash);

  // Editing the declared revision defeats the pin; the lock must report it and
  // `--locked` must refuse to build.
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "v2"));
  auto edited = request_for({load_manifest(app_root)}, app_root);
  edited.git_cache = cache;
  edited.existing_lock = &lock;
  auto third = COVA_REQUIRE_OK(cova::package::resolve(edited));
  COVA_CHECK_NE(*third.find("math")->git_rev, pinned);
  auto outcome = cova::package::enforce_lock(third, app_root / "cova.lock", /*locked=*/true);
  COVA_CHECK(!outcome.has_value());
}

COVA_TEST_NAMED(package, first_resolution_reads_the_remote_instead_of_a_cached_head) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  const auto cache = base / "git-cache";
  init_library_repo(lib_root, "math");
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "master"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = cache;
  auto first = COVA_REQUIRE_OK(cova::package::resolve(request));
  const std::string pinned = *first.find("math")->git_rev;

  COVA_CHECK(cova::fs::write_file(lib_root / "src" / "source.cpp",
                                  "int value() { return 2; }\n")
                 .has_value());
  commit_all(lib_root, "second");

  // A second project shares the cache but has no lockfile yet. It has no pin to
  // reuse, so it must read the branch instead of inheriting the first project's
  // cached head and silently locking a stale commit.
  const auto other_root = base / "other";
  write_project(other_root, "other", "executable", git_dependency("math", lib_root, "master"));
  auto other = request_for({load_manifest(other_root)}, other_root);
  other.git_cache = cache;
  auto second = COVA_REQUIRE_OK(cova::package::resolve(other));
  COVA_CHECK_NE(*second.find("math")->git_rev, pinned);
}

COVA_TEST_NAMED(package, resolves_a_relative_git_url_against_the_manifest) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  init_library_repo(lib_root, "math");
  git_run(lib_root, {"tag", "v1"});
  // `../lib` is relative to the manifest, not to the process working directory.
  write_project(app_root, "app", "executable", git_dependency("math", "../lib", "v1"));

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = base / "git-cache";
  auto graph = COVA_REQUIRE_OK(cova::package::resolve(request));

  const auto* math = graph.find("math");
  COVA_CHECK(math != nullptr);
  COVA_CHECK(math->source_url.has_value());
  // The resolved source is absolute, so the cache key and the lockfile do not
  // depend on where Cova was started, and two projects cannot share a cache
  // entry that means different repositories.
  COVA_CHECK_EQ(*math->source_url,
                cova::fs::generic_string(cova::fs::absolute_normalized(lib_root)));
  COVA_CHECK(cova::fs::is_regular_file(math->manifest.root / "cova.toml"));
}

COVA_TEST_NAMED(package, rejects_two_git_sources_for_the_same_package) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  const auto extra_root = base / "extra";
  init_library_repo(lib_root, "math");
  git_run(lib_root, {"tag", "v1"});
  COVA_CHECK(cova::fs::write_file(lib_root / "src" / "source.cpp",
                                  "int value() { return 1; }\n")
                 .has_value());
  commit_all(lib_root, "second");
  git_run(lib_root, {"tag", "v2"});

  // `app` asks for v1 and its path dependency asks for v2: one package name
  // resolved to two commits, which cannot be built.
  write_project(extra_root, "extra", "static-library", git_dependency("math", lib_root, "v2"));
  const std::string dependencies =
      git_dependency("math", lib_root, "v1") + "extra = { path = \"../extra\" }\n";
  write_project(app_root, "app", "executable", dependencies);

  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = base / "git-cache";
  auto error = COVA_REQUIRE_ERR(cova::package::resolve(request));
  COVA_CHECK_EQ(error.code, std::string("cova::package::duplicate"));
  COVA_CHECK_MSG(error.message.find("two different git sources") != std::string::npos,
                 error.message);
  // The diagnostic names the sources, never the cache-internal checkout paths.
  bool leaks_cache = false;
  for (const auto& line : error.context) {
    leaks_cache = leaks_cache || line.find("checkouts") != std::string::npos;
  }
  COVA_CHECK(!leaks_cache);
}

COVA_TEST_NAMED(package, git_source_hash_ignores_checkout_line_endings) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  init_library_repo(lib_root, "math");
  git_run(lib_root, {"tag", "v1"});
  write_project(app_root, "app", "executable", git_dependency("math", lib_root, "v1"));

  const auto crlf_false = base / "gitconfig-false";
  const auto crlf_true = base / "gitconfig-true";
  COVA_CHECK(cova::fs::write_file(crlf_false, "[core]\n\tautocrlf = false\n").has_value());
  COVA_CHECK(cova::fs::write_file(crlf_true, "[core]\n\tautocrlf = true\n").has_value());

  auto hash_with = [&](const stdfs::path& config, const stdfs::path& cache) {
    COVA_CHECK(
        cova::fs::set_env("GIT_CONFIG_GLOBAL", cova::fs::generic_string(config)).has_value());
    auto request = request_for({load_manifest(app_root)}, app_root);
    request.git_cache = cache;
    auto graph = COVA_REQUIRE_OK(cova::package::resolve(request));
    return graph.find("math")->source_hash;
  };
  const std::string hash_lf = hash_with(crlf_false, base / "cache-lf");
  const std::string hash_crlf = hash_with(crlf_true, base / "cache-crlf");
  // Leave the process with a valid config even when the developer had none.
  const auto previous = cova::fs::get_env("GIT_CONFIG_GLOBAL");
  const std::string restore =
      previous.value_or(cova::fs::generic_string(crlf_false));
  COVA_CHECK(cova::fs::set_env("GIT_CONFIG_GLOBAL", restore).has_value());

  // The hash is taken from the commit's tree, not from the checkout, so a
  // checkout with CRLF line endings (Git for Windows' default) cannot make a
  // lockfile written on another platform look stale.
  COVA_CHECK_EQ(hash_lf, hash_crlf);
}

COVA_TEST_NAMED(package, resolves_a_default_branch_that_is_not_main_or_master) {
  if (!have_git()) {
    return;
  }
  cova::test::TempDir temp;
  const stdfs::path base(temp.path());
  const auto lib_root = base / "lib";
  const auto app_root = base / "app";
  init_library_repo(lib_root, "math");
  // The repository has two branches and its default is `trunk`; a `rev`-less
  // dependency must follow the remote HEAD, not guess main or master.
  git_run(lib_root, {"branch", "trunk"});
  git_run(lib_root, {"checkout", "--quiet", "trunk"});
  COVA_CHECK(cova::fs::write_file(lib_root / "src" / "source.cpp",
                                  "int value() { return 9; }\n")
                 .has_value());
  commit_all(lib_root, "trunk work");
  const std::string trunk_commit = git_output(lib_root, {"rev-parse", "HEAD"});

  write_project(app_root, "app", "executable", git_dependency("math", lib_root));
  auto request = request_for({load_manifest(app_root)}, app_root);
  request.git_cache = base / "git-cache";
  auto graph = COVA_REQUIRE_OK(cova::package::resolve(request));
  COVA_CHECK_EQ(*graph.find("math")->git_rev, trunk_commit);
}

COVA_TEST_NAMED(package, redacts_credentials_from_recorded_urls) {
  using cova::package::git::redact_url;
  COVA_CHECK_EQ(redact_url("https://user:token@example.com/x.git"),
                std::string("https://***@example.com/x.git"));
  COVA_CHECK_EQ(redact_url("https://ghp_secret@example.com/x.git"),
                std::string("https://***@example.com/x.git"));
  // The conventional SSH account carries no secret and stays readable.
  COVA_CHECK_EQ(redact_url("ssh://git@example.com/x.git"),
                std::string("ssh://git@example.com/x.git"));
  COVA_CHECK_EQ(redact_url("https://example.com/x.git"),
                std::string("https://example.com/x.git"));
  COVA_CHECK_EQ(redact_url("../lib"), std::string("../lib"));
}

COVA_TEST_MAIN()
