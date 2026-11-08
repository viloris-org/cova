// Cova — managed toolchain installer tests (PRD §7.2, §8.6, §18.1).
#include <filesystem>
#include <fstream>
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"
#include "core/process/process.hpp"
#include "testing/test.hpp"
#include "toolchain/installer/installer.hpp"

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

void write_driver(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path);
  stream << "#!/bin/sh\n"
            "case \"$1\" in\n"
            "  --version) echo 'g++ (GCC) 14.2.0' ;;\n"
            "  -print-resource-dir) dirname \"$0\" ;;\n"
            "  *) exit 0 ;;\n"
            "esac\n";
  stream.close();
  std::error_code ec;
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add, ec);
}

struct Fixture {
  cova::test::TempDir temp;
  std::filesystem::path base = std::filesystem::path(temp.path());
  std::filesystem::path registry = base / "registry";
  std::filesystem::path archive = registry / "archives" / "fake-cross.tar.gz";
  std::string digest;

  bool build() {
    if (!cova::fs::find_in_path("tar").has_value()) {
      return false;
    }
    const auto stage = base / "stage" / "fake-cross";
    write_driver(stage / "bin" / "aarch64-linux-gnu-g++");
    if (!cova::fs::write_file(stage / "toolchain.toml",
                              "id = \"fake-cross\"\n"
                              "family = \"gcc\"\n"
                              "version = \"14.2.0\"\n"
                              "target = \"aarch64-unknown-linux-gnu\"\n"
                              "compiler = \"bin/aarch64-linux-gnu-g++\"\n")) {
      return false;
    }
    if (!cova::fs::create_directories(archive.parent_path())) {
      return false;
    }
    auto tar = cova::proc::run_line("tar -czf " + quote_shell(archive.string()) + " -C " +
                                    quote_shell((base / "stage").string()) + " fake-cross");
    if (!tar || !tar->success()) {
      return false;
    }
    digest = *cova::Sha256::hex_of_file(archive);
    const std::string index = "[[toolchain]]\n"
                              "id = \"fake-cross\"\n"
                              "name = \"fake-cross\"\n"
                              "family = \"gcc\"\n"
                              "version = \"14.2.0\"\n"
                              "target = \"aarch64-unknown-linux-gnu\"\n"
                              "archive = \"archives/fake-cross.tar.gz\"\n"
                              "sha256 = \"" +
                              digest +
                              "\"\n"
                              "size-bytes = " +
                              std::to_string(cova::fs::file_size(archive)) +
                              "\n"
                              "compiler = \"bin/aarch64-linux-gnu-g++\"\n"
                              "license = \"GPL-3.0-or-later\"\n";
    return cova::fs::write_file(registry / "index.toml", index).has_value();
  }

  cova::toolchain::ToolchainInstallOptions options(const std::string& toolchains = "toolchains") {
    cova::toolchain::ToolchainInstallOptions out;
    out.registry_root = registry;
    out.toolchains_root = base / toolchains;
    out.downloads_root = base / "downloads";
    out.allow_install = true;
    return out;
  }
};

} // namespace

COVA_TEST_NAMED(toolchain, installs_a_verified_registry_archive) {
  Fixture fixture;
  if (!fixture.build()) {
    return; // tar is required to build the fixture
  }
  cova::toolchain::ToolchainInstaller installer(fixture.options());
  auto available = COVA_REQUIRE_OK(installer.available());
  COVA_CHECK_EQ(available.size(), std::size_t{1});
  COVA_CHECK_EQ(available.front().id, std::string("fake-cross"));

  auto installed = COVA_REQUIRE_OK(installer.install("fake-cross"));
  COVA_CHECK_EQ(installed.descriptor.family, std::string("gcc"));
  COVA_CHECK_EQ(installed.descriptor.version.to_string(), std::string("14.2.0"));
  COVA_CHECK_EQ(installed.descriptor.registry_id, std::string("fake-cross"));
  COVA_CHECK_EQ(installed.descriptor.trust_level, std::string("verified"));
  COVA_CHECK(cova::fs::is_regular_file(installed.root / "bin" / "aarch64-linux-gnu-g++"));
  COVA_CHECK(cova::fs::is_regular_file(installed.root / "toolchain.toml"));

  // Installing again is idempotent and re-verifies the installed tree.
  auto again = COVA_REQUIRE_OK(installer.install("fake-cross"));
  COVA_CHECK_EQ(again.descriptor.content_hash, installed.descriptor.content_hash);
  auto verified = COVA_REQUIRE_OK(installer.verify("fake-cross"));
  COVA_CHECK_EQ(verified.entry.sha256, fixture.digest);
}

COVA_TEST_NAMED(toolchain, installer_honours_offline_and_checks_the_hash) {
  Fixture fixture;
  if (!fixture.build()) {
    return;
  }
  auto offline_options = fixture.options("offline-toolchains");
  offline_options.offline = true;
  cova::toolchain::ToolchainInstaller offline(offline_options);
  auto refused = offline.install("fake-cross");
  COVA_CHECK(!refused.has_value());
  COVA_CHECK(refused.error().code.find("offline") != std::string::npos);

  // A tampered archive must fail before anything is committed.
  auto handle = std::ofstream(fixture.archive, std::ios::binary | std::ios::app);
  handle << "tampered";
  handle.close();
  cova::toolchain::ToolchainInstaller installer(fixture.options("toolchains"));
  auto tampered = installer.install("fake-cross");
  COVA_CHECK(!tampered.has_value());
  COVA_CHECK(tampered.error().code.find("verification") != std::string::npos);
  COVA_CHECK(!cova::fs::is_directory(fixture.base / "toolchains" / "fake-cross"));
}

COVA_TEST_MAIN()
