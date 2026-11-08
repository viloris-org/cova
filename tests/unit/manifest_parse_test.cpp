// Cova — manifest parsing and round-trip tests (PRD §5.3).
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "manifest/manifest.hpp"
#include "testing/test.hpp"

namespace {

constexpr std::string_view kManifest = R"(
[package]
name = "hello"
version = "0.2.0"
edition = "c++26"

[build]
type = "executable"
sources = ["src/**/*.cpp"]
public-includes = ["include"]
compile-options = ["-Wall"]

[dependencies]
fmt = "11"
local = { path = "vendor/local" }

[profile.release]
optimization = 3
lto = true
assertions = false

[toolchain]
policy = "system-first"
compiler = "clang"
version = ">=20, <24"

[format]
tool = "clang-format"
style = "file"
)";

}  // namespace

COVA_TEST_NAMED(manifest, parses_sections) {
  cova::test::TempDir temp;
  auto loaded = COVA_REQUIRE_OK(
      cova::manifest::parse(kManifest, "cova.toml", temp.path()));
  const auto& manifest = loaded.manifest;
  COVA_CHECK_EQ(manifest.package.name, std::string("hello"));
  COVA_CHECK_EQ(manifest.package.version, std::string("0.2.0"));
  COVA_CHECK(manifest.build.kind == cova::manifest::PackageKind::Executable);
  COVA_CHECK_EQ(manifest.build.sources.size(), std::size_t{1});
  COVA_CHECK_EQ(manifest.dependencies.size(), std::size_t{2});
  const auto& fmt = manifest.dependencies.front();
  COVA_CHECK_EQ(fmt.name, std::string("fmt"));
  COVA_CHECK(fmt.requirements.front().matches(cova::Version{11, 5, 0}));
  COVA_CHECK(!fmt.requirements.front().matches(cova::Version{12, 0, 0}));
  COVA_CHECK(manifest.dependencies.back().path.has_value());
  COVA_CHECK(manifest.profiles.contains("release"));
  COVA_CHECK(manifest.profiles.at("release").lto);
  COVA_CHECK_EQ(manifest.toolchain.compiler, std::string("clang"));
}

COVA_TEST_NAMED(manifest, round_trips_through_toml) {
  cova::test::TempDir temp;
  auto loaded = COVA_REQUIRE_OK(
      cova::manifest::parse(kManifest, "cova.toml", temp.path()));
  const std::string rendered = cova::manifest::to_toml(loaded.manifest);
  auto reparsed = COVA_REQUIRE_OK(
      cova::manifest::parse(rendered, "cova.toml", temp.path()));
  COVA_CHECK_EQ(reparsed.manifest.package.name, std::string("hello"));
  COVA_CHECK(reparsed.manifest.build.kind == loaded.manifest.build.kind);
  COVA_CHECK(reparsed.manifest.build.sources == loaded.manifest.build.sources);
  COVA_CHECK_EQ(reparsed.manifest.dependencies.size(), std::size_t{2});
  COVA_CHECK(reparsed.manifest.profiles.at("release").lto);
}

COVA_TEST_NAMED(manifest, default_manifest_is_valid) {
  auto manifest = cova::manifest::default_manifest("demo",
                                                   cova::manifest::PackageKind::StaticLibrary);
  auto warnings = cova::manifest::validate(manifest);
  COVA_CHECK_MSG(warnings.has_value(), warnings ? "" : warnings.error().to_string());
  COVA_CHECK(manifest.build.kind == cova::manifest::PackageKind::StaticLibrary);
  COVA_CHECK(!manifest.build.public_includes.empty());
  COVA_CHECK(!manifest.test.sources.empty());
}

COVA_TEST_NAMED(manifest, rejects_invalid_names) {
  auto manifest = cova::manifest::default_manifest("demo",
                                                   cova::manifest::PackageKind::Executable);
  manifest.package.name = "9bad name";
  auto error = COVA_REQUIRE_ERR(cova::manifest::validate(manifest));
  COVA_CHECK(error.code.find("manifest") != std::string::npos);
}

COVA_TEST_NAMED(manifest, load_reports_readable_errors) {
  cova::test::TempDir temp;
  const auto path = std::filesystem::path(temp.path()) / "missing" / "cova.toml";
  auto error = COVA_REQUIRE_ERR(cova::manifest::load(path));
  COVA_CHECK(error.code.find("manifest") != std::string::npos);
}

COVA_TEST_NAMED(manifest, workspace_manifest_has_no_package_requirement) {
  auto manifest = cova::manifest::default_workspace_manifest("engine");
  COVA_CHECK(manifest.is_workspace_root());
  COVA_CHECK(!manifest.has_package());
  COVA_CHECK(!manifest.workspace->members.empty());
}

COVA_TEST_NAMED(manifest, rejects_unimplemented_build_scripts) {
  cova::test::TempDir temp;
  auto error = COVA_REQUIRE_ERR(cova::manifest::parse(R"(
[package]
name = "scripted"
version = "0.1.0"

[build]
type = "executable"
sources = ["src/*.cpp"]
build-script = true
)",
                                                       "cova.toml", temp.path()));
  COVA_CHECK_EQ(error.code, std::string("cova::manifest::build_script_unsupported"));
  COVA_CHECK(error.message.find("build-script") != std::string::npos);
}

COVA_TEST_MAIN()
