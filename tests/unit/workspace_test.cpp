// Cova — workspace discovery and ordering tests (PRD §14).
#include <algorithm>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "testing/test.hpp"
#include "workspace/workspace.hpp"

namespace {

constexpr std::string_view kWorkspaceManifest = R"(
[workspace]
members = ["packages/*"]
)";

constexpr std::string_view kMemberManifest = R"(
[package]
name = "MEMBER"
version = "0.1.0"
edition = "c++26"

[build]
type = "TYPE"
sources = ["src/**/*.cpp"]
)";

void write_member(const std::filesystem::path& root, std::string_view name,
                  std::string_view type) {
  COVA_CHECK(cova::fs::create_directories(root / "src").has_value());
  COVA_CHECK(cova::fs::write_file(root / "src" / "main.cpp", "int main() { return 0; }\n")
                 .has_value());
  std::string manifest = std::string(kMemberManifest);
  manifest = cova::str::replace_all(manifest, "MEMBER", name);
  manifest = cova::str::replace_all(manifest, "TYPE", type);
  COVA_CHECK(cova::fs::write_file(root / "cova.toml", manifest).has_value());
}

}  // namespace

COVA_TEST_NAMED(workspace, loads_globbed_members) {
  cova::test::TempDir temp;
  const auto root = std::filesystem::path(temp.path()) / "ws";
  COVA_CHECK(cova::fs::create_directories(root).has_value());
  COVA_CHECK(cova::fs::write_file(root / "cova.toml", kWorkspaceManifest).has_value());
  write_member(root / "packages" / "alpha", "alpha", "executable");
  write_member(root / "packages" / "beta", "beta", "static-library");

  auto workspace = COVA_REQUIRE_OK(cova::workspace::load(root));
  COVA_CHECK_EQ(workspace.members.size(), std::size_t{2});
  COVA_CHECK(workspace.find_member("alpha") != nullptr);
  COVA_CHECK(workspace.find_member("beta") != nullptr);
  const auto relpaths = workspace.member_relpaths();
  COVA_CHECK_EQ(relpaths.size(), std::size_t{2});
}

COVA_TEST_NAMED(workspace, build_order_is_deterministic) {
  cova::test::TempDir temp;
  const auto root = std::filesystem::path(temp.path()) / "ws";
  COVA_CHECK(cova::fs::create_directories(root).has_value());
  COVA_CHECK(cova::fs::write_file(root / "cova.toml", kWorkspaceManifest).has_value());
  write_member(root / "packages" / "aaa", "aaa", "executable");
  write_member(root / "packages" / "zzz", "zzz", "static-library");
  auto workspace = COVA_REQUIRE_OK(cova::workspace::load(root));
  auto order = COVA_REQUIRE_OK(cova::workspace::member_build_order(workspace));
  COVA_CHECK_EQ(order.size(), std::size_t{2});
  COVA_CHECK_EQ(order.front(), std::string("aaa"));
  COVA_CHECK_EQ(order.back(), std::string("zzz"));
}

COVA_TEST_NAMED(workspace, rejects_non_workspace_manifest) {
  cova::test::TempDir temp;
  const auto root = std::filesystem::path(temp.path()) / "plain";
  COVA_CHECK(cova::fs::create_directories(root).has_value());
  COVA_CHECK(cova::fs::write_file(root / "cova.toml", R"(
[package]
name = "plain"
[build]
sources = ["src/**/*.cpp"]
)")
                 .has_value());
  auto error = COVA_REQUIRE_ERR(cova::workspace::load(root));
  COVA_CHECK(error.code.find("workspace") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Exclusion matching
// ---------------------------------------------------------------------------

namespace {

// Creates a workspace with one package per entry in `members` (name == dir),
// plus the given exclude list, and returns the discovered member names.
std::vector<std::string> discover(const std::filesystem::path& root,
                                 const std::vector<std::string>& member_globs,
                                 const std::vector<std::string>& excludes,
                                 const std::vector<std::string>& directories) {
  for (const auto& directory : directories) {
    const auto package_dir = root / directory;
    COVA_CHECK(cova::fs::create_directories(package_dir).has_value());
    COVA_CHECK(cova::fs::write_file(package_dir / "cova.toml",
                                    "[package]\nname = \"" +
                                        std::filesystem::path(directory).filename().string() +
                                        "\"\nversion = \"0.1.0\"\n")
                   .has_value());
  }
  std::string manifest = "[workspace]\nmembers = [";
  for (std::size_t index = 0; index < member_globs.size(); ++index) {
    manifest += (index == 0 ? "\"" : ", \"") + member_globs[index] + "\"";
  }
  manifest += "]\nexclude = [";
  for (std::size_t index = 0; index < excludes.size(); ++index) {
    manifest += (index == 0 ? "\"" : ", \"") + excludes[index] + "\"";
  }
  manifest += "]\n";
  COVA_CHECK(cova::fs::write_file(root / "cova.toml", manifest).has_value());

  auto workspace = cova::workspace::load(root);
  COVA_CHECK_MSG(workspace.has_value(), "workspace failed to load");
  std::vector<std::string> names;
  for (const auto& member : workspace->members) {
    names.push_back(member.name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

COVA_TEST_NAMED(workspace, exclude_matches_whole_path_components_only) {
  cova::test::TempDir temp;
  // Excluding "vendor" must not also remove "vendor-lib": a substring match
  // used to delete it from the workspace without any diagnostic.
  const auto names = discover(temp.path(), {"vendor", "vendor-lib", "alpha"}, {"vendor"},
                              {"vendor", "vendor-lib", "alpha"});
  COVA_CHECK_EQ(names.size(), std::size_t{2});
  COVA_CHECK(std::find(names.begin(), names.end(), "vendor-lib") != names.end());
  COVA_CHECK(std::find(names.begin(), names.end(), "alpha") != names.end());
  COVA_CHECK(std::find(names.begin(), names.end(), "vendor") == names.end());
}

COVA_TEST_NAMED(workspace, exclude_does_not_behave_like_a_substring_filter) {
  cova::test::TempDir temp;
  // A short exclusion used to drop every member whose path merely contained it.
  const auto names = discover(temp.path(), {"alpha", "beta"}, {"beta"}, {"alpha", "beta"});
  COVA_CHECK_EQ(names.size(), std::size_t{1});
  COVA_CHECK_EQ(names.front(), std::string("alpha"));
}

COVA_TEST_NAMED(workspace, exclude_supports_glob_components) {
  cova::test::TempDir temp;
  const auto names = discover(temp.path(), {"*"}, {"build-*"}, {"build-tools", "app"});
  COVA_CHECK_EQ(names.size(), std::size_t{1});
  COVA_CHECK_EQ(names.front(), std::string("app"));
}

COVA_TEST_MAIN()
