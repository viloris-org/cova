// Cova — sysroot manifest, verification and safe extraction tests (PRD §8).
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "sysroot/manager/manager.hpp"
#include "testing/test.hpp"

COVA_TEST_NAMED(sysroot, manifest_round_trips) {
  cova::sysroot::SysrootManifest manifest;
  manifest.id = "aarch64-musl-1";
  manifest.name = "aarch64-musl";
  manifest.target = COVA_REQUIRE_OK(cova::platform::Triple::parse("aarch64-unknown-linux-musl"));
  manifest.crt = cova::platform::Crt::Musl;
  manifest.libcpp = cova::platform::LibCpp::LibStdCpp;
  manifest.abi = cova::platform::Abi::Musl;
  manifest.include_dirs = {"include"};
  manifest.lib_dirs = {"lib"};
  manifest.dynamic_linker = "lib/ld-musl-aarch64.so.1";
  manifest.sha256 = "abc";

  auto parsed = COVA_REQUIRE_OK(cova::sysroot::SysrootManifest::from_toml(manifest.to_toml()));
  COVA_CHECK_EQ(parsed.id, std::string("aarch64-musl-1"));
  COVA_CHECK(parsed.target == manifest.target);
  COVA_CHECK(parsed.crt == cova::platform::Crt::Musl);
  COVA_CHECK_EQ(parsed.include_dirs.size(), std::size_t{1});
}

COVA_TEST_NAMED(sysroot, verifies_a_complete_tree) {
  cova::test::TempDir temp;
  const auto root = std::filesystem::path(temp.path()) / "root";
  COVA_CHECK(cova::fs::create_directories(root / "include").has_value());
  COVA_CHECK(cova::fs::create_directories(root / "lib").has_value());
  cova::sysroot::SysrootManifest manifest;
  manifest.id = "test";
  manifest.name = "test";
  manifest.target = cova::platform::host_triple();
  manifest.include_dirs = {"include"};
  manifest.lib_dirs = {"lib"};
  cova::sysroot::VerifyReport report;
  COVA_CHECK(cova::sysroot::verify(root, manifest, report).has_value());
  COVA_CHECK(report.ok);
  COVA_CHECK(!report.findings.empty());

  manifest.include_dirs = {"missing-include"};
  report = {};
  COVA_CHECK(cova::sysroot::verify(root, manifest, report).has_value());
  COVA_CHECK(!report.ok);
}

COVA_TEST_NAMED(sysroot, manager_lists_installed) {
  cova::test::TempDir temp;
  const auto sysroots = std::filesystem::path(temp.path()) / "sysroots";
  const auto installed = sysroots / "test-sysroot";
  COVA_CHECK(cova::fs::create_directories(installed / "root" / "include").has_value());
  cova::sysroot::SysrootManifest manifest;
  manifest.id = "test-sysroot";
  manifest.name = "test-sysroot";
  manifest.target = cova::platform::host_triple();
  manifest.include_dirs = {"include"};
  COVA_CHECK(cova::fs::write_file(installed / "sysroot.toml", manifest.to_toml()).has_value());

  cova::sysroot::ManagerOptions options;
  options.sysroots_root = sysroots;
  cova::sysroot::SysrootManager manager(options);
  auto entries = COVA_REQUIRE_OK(manager.list());
  COVA_CHECK_EQ(entries.size(), std::size_t{1});
  COVA_CHECK_EQ(entries.front().manifest.id, std::string("test-sysroot"));
  auto found = COVA_REQUIRE_OK(manager.find("test-sysroot"));
  COVA_CHECK_EQ(found.manifest.id, std::string("test-sysroot"));
}

COVA_TEST_MAIN()
