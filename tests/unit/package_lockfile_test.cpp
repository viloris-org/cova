// Cova — lockfile round-trip and identity tests (PRD §15).
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "package/lockfile/lockfile.hpp"
#include "testing/test.hpp"

COVA_TEST_NAMED(package, dependency_lock_round_trips) {
  cova::test::TempDir temp;
  cova::lockfile::DependencyLock lock;
  lock.generated_by = "cova 0.1.0";
  lock.manifest_fingerprint = "abc";
  cova::lockfile::LockedPackage package;
  package.name = "fmt";
  package.version = "11.0.2";
  package.registry = "default";
  package.source_hash = "deadbeef";
  package.dependencies = {"base"};
  package.features = {"header-only"};
  lock.packages.push_back(package);

  const std::string rendered = lock.to_toml();
  auto parsed = COVA_REQUIRE_OK(cova::lockfile::DependencyLock::from_toml(rendered));
  COVA_CHECK_EQ(parsed.packages.size(), std::size_t{1});
  COVA_CHECK_EQ(parsed.packages.front().name, std::string("fmt"));
  COVA_CHECK_EQ(parsed.packages.front().version, std::string("11.0.2"));
  COVA_CHECK(parsed.find("fmt") != nullptr);
  COVA_CHECK(parsed.find("missing") == nullptr);

  const auto path = std::filesystem::path(temp.path()) / "cova.lock";
  COVA_CHECK(lock.save(path).has_value());
  auto loaded = COVA_REQUIRE_OK(cova::lockfile::DependencyLock::load(path));
  COVA_CHECK_EQ(loaded.packages.size(), std::size_t{1});
}

COVA_TEST_NAMED(package, toolchain_lock_round_trips) {
  cova::lockfile::ToolchainLock lock;
  lock.generated_by = "cova 0.1.0";
  cova::lockfile::LockedToolchain entry;
  entry.target = "x86_64-unknown-linux-gnu";
  entry.toolchain_id = "system-gcc-16.2.1";
  entry.toolchain_family = "gcc";
  entry.toolchain_version = "16.2.1";
  entry.content_hash = "hash";
  entry.source = "system";
  entry.host = "x86_64-unknown-linux-gnu";
  entry.abi_key = "abi";
  entry.components = {"cxx=/usr/bin/g++"};
  lock.entries.push_back(entry);

  auto parsed = COVA_REQUIRE_OK(cova::lockfile::ToolchainLock::from_toml(lock.to_toml()));
  COVA_CHECK_EQ(parsed.entries.size(), std::size_t{1});
  COVA_CHECK(parsed.for_target("x86_64-unknown-linux-gnu") != nullptr);
  COVA_CHECK(parsed.for_target("aarch64-unknown-linux-musl") == nullptr);
  COVA_CHECK_EQ(parsed.entries.front().toolchain_id, std::string("system-gcc-16.2.1"));
}

COVA_TEST_NAMED(package, manifest_fingerprint_is_deterministic) {
  const std::string first = cova::lockfile::manifest_fingerprint("a", {"x", "y"});
  const std::string second = cova::lockfile::manifest_fingerprint("a", {"y", "x"});
  COVA_CHECK_EQ(first, second);
  COVA_CHECK(first != cova::lockfile::manifest_fingerprint("b", {"y", "x"}));
}

COVA_TEST_MAIN()
