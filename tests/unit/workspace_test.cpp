// Cova — workspace discovery and ordering tests (PRD §14).
#include <string>

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

COVA_TEST_MAIN()
