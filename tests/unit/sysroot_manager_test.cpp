// Cova — sysroot manifest, verification and safe extraction tests (PRD §8).
#include <filesystem>
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"
#include "core/process/process.hpp"
#include "sysroot/manager/manager.hpp"
#include "testing/test.hpp"

namespace {

std::string quote_shell(std::string_view value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

} // namespace

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

COVA_TEST_NAMED(sysroot, registry_installs_verified_archive) {
  if (!cova::fs::find_in_path("tar").has_value()) {
    return; // archive creation is part of the fixture
  }
  cova::test::TempDir temp;
  const auto base = std::filesystem::path(temp.path());
  const auto stage = base / "stage" / "aarch64-linux-musl";
  COVA_CHECK(cova::fs::create_directories(stage / "usr" / "include").has_value());
  COVA_CHECK(cova::fs::create_directories(stage / "usr" / "lib").has_value());
  COVA_CHECK(cova::fs::write_file(stage / "usr" / "include" / "stdio.h", "").has_value());

  cova::sysroot::SysrootManifest manifest;
  manifest.id = "test-musl";
  manifest.name = "test-musl";
  manifest.target = *cova::platform::Triple::parse("aarch64-unknown-linux-musl");
  manifest.crt = cova::platform::Crt::Musl;
  manifest.include_dirs = {"usr/include"};
  manifest.lib_dirs = {"usr/lib"};
  COVA_CHECK(cova::fs::write_file(stage / "sysroot.toml", manifest.to_toml()).has_value());

  const auto registry = base / "registry";
  const auto archive = registry / "archives" / "test-musl.tar.gz";
  COVA_CHECK(cova::fs::create_directories(archive.parent_path()).has_value());
  auto tar = cova::proc::run_line("tar -czf " + quote_shell(archive.string()) + " -C " +
                                  quote_shell((base / "stage").string()) + " aarch64-linux-musl");
  COVA_CHECK_MSG(tar.has_value() && tar->success(), "could not create the test archive");

  const std::string digest = *cova::Sha256::hex_of_file(archive);
  const std::string index = "[[sysroot]]\n"
                            "id = \"test-musl\"\n"
                            "name = \"test-musl\"\n"
                            "target = \"aarch64-unknown-linux-musl\"\n"
                            "crt = \"musl\"\n"
                            "archive = \"archives/test-musl.tar.gz\"\n"
                            "sha256 = \"" +
                            digest +
                            "\"\n"
                            "size-bytes = " +
                            std::to_string(cova::fs::file_size(archive)) + "\n";
  COVA_CHECK(cova::fs::write_file(registry / "index.toml", index).has_value());

  cova::sysroot::ManagerOptions options;
  options.sysroots_root = base / "sysroots";
  options.store_root = base / "store";
  options.downloads_root = base / "downloads";
  options.registry_root = registry;
  options.allow_install = true;
  cova::sysroot::SysrootManager manager(options);

  auto installed = COVA_REQUIRE_OK(
      manager.install(*cova::platform::Triple::parse("aarch64-unknown-linux-musl"), "test-musl"));
  COVA_CHECK_EQ(installed.manifest.id, std::string("test-musl"));
  COVA_CHECK(installed.manifest.target ==
             *cova::platform::Triple::parse("aarch64-unknown-linux-musl"));
  COVA_CHECK_EQ(installed.manifest.sha256, digest);
  COVA_CHECK(cova::fs::is_directory(installed.root / "usr" / "include"));

  // Offline mode must refuse before touching the archive.
  cova::sysroot::ManagerOptions offline = options;
  offline.offline = true;
  cova::sysroot::SysrootManager offline_manager(offline);
  auto refused = offline_manager.install(
      *cova::platform::Triple::parse("aarch64-unknown-linux-musl"), "test-musl");
  COVA_CHECK(!refused.has_value());
  COVA_CHECK(refused.error().code.find("offline") != std::string::npos);
}

COVA_TEST_NAMED(sysroot, registry_rejects_tampered_archive) {
  cova::test::TempDir temp;
  const auto base = std::filesystem::path(temp.path());
  const auto registry = base / "registry";
  const auto archive = registry / "broken.tar.gz";
  COVA_CHECK(cova::fs::create_directories(registry).has_value());
  COVA_CHECK(cova::fs::write_file(archive, "not the archive you hashed").has_value());
  const std::string index =
      "[[sysroot]]\n"
      "id = \"broken\"\n"
      "name = \"broken\"\n"
      "target = \"aarch64-unknown-linux-musl\"\n"
      "archive = \"broken.tar.gz\"\n"
      "sha256 = "
      "\"0000000000000000000000000000000000000000000000000000000000000000\"\n";
  COVA_CHECK(cova::fs::write_file(registry / "index.toml", index).has_value());

  cova::sysroot::ManagerOptions options;
  options.sysroots_root = base / "sysroots";
  options.store_root = base / "store";
  options.downloads_root = base / "downloads";
  options.registry_root = registry;
  cova::sysroot::SysrootManager manager(options);
  auto refused =
      manager.install(*cova::platform::Triple::parse("aarch64-unknown-linux-musl"), "broken");
  COVA_CHECK(!refused.has_value());
  COVA_CHECK(refused.error().code.find("verification") != std::string::npos);
}

COVA_TEST_MAIN()
