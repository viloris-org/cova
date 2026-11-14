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

// ---------------------------------------------------------------------------
// Round-trip fidelity
//
// `to_toml` is used by `cova new` and by any command that rewrites the
// manifest. Omitting a key that has a built-in default silently re-applied the
// default on the next parse, so a value the user wrote could change without
// them touching the file.
// ---------------------------------------------------------------------------

namespace {

// Parses `text`, serialises it, re-parses the result and returns the manifest.
cova::manifest::Manifest round_trip(std::string_view text, const std::filesystem::path& root) {
  auto first = cova::manifest::parse(text, "cova.toml", root);
  COVA_CHECK_MSG(first.has_value(), "input manifest failed to parse");
  const std::string rendered = cova::manifest::to_toml(first->manifest);
  auto second = cova::manifest::parse(rendered, "cova.toml", root);
  COVA_CHECK_MSG(second.has_value(), "rendered manifest failed to parse");
  return second->manifest;
}

constexpr const char* kPackageHeader =
    "[package]\nname = \"rt\"\nversion = \"0.1.0\"\nedition = \"c++26\"\n";

}  // namespace

COVA_TEST_NAMED(manifest, round_trip_preserves_explicit_profile_flags) {
  cova::test::TempDir temp;
  const auto manifest = round_trip(std::string(kPackageHeader) +
                                       "[profile.release]\nlto = false\nassertions = true\n",
                                   temp.path());
  const auto& release = manifest.profiles.at("release");
  COVA_CHECK_MSG(!release.lto, "release lto=false must survive a rewrite");
  COVA_CHECK_MSG(release.assertions, "release assertions=true must survive a rewrite");
}

COVA_TEST_NAMED(manifest, round_trip_preserves_workspace_toolchain_pin) {
  cova::test::TempDir temp;
  const auto manifest = round_trip(
      "[workspace]\nmembers = [\"a\"]\n"
      "[workspace.toolchain]\npolicy = \"managed-first\"\nauto-install = false\n"
      "preferred = \"/opt/llvm/bin/clang++\"\n",
      temp.path());
  COVA_CHECK(manifest.workspace.has_value());
  COVA_CHECK(manifest.workspace->toolchain.has_value());
  COVA_CHECK(!manifest.workspace->toolchain->auto_install);
  COVA_CHECK_EQ(manifest.workspace->toolchain->preferred, std::string("/opt/llvm/bin/clang++"));
}

COVA_TEST_NAMED(manifest, round_trip_preserves_workspace_format_settings) {
  cova::test::TempDir temp;
  const auto manifest =
      round_trip("[workspace]\nmembers = [\"a\"]\n"
                 "[workspace.format]\nfallback-style = \"Google\"\n",
                 temp.path());
  COVA_CHECK(manifest.workspace->format.has_value());
  COVA_CHECK_EQ(manifest.workspace->format->fallback_style, std::string("Google"));
}

COVA_TEST_NAMED(manifest, explicit_empty_test_sources_disable_discovery) {
  cova::test::TempDir temp;
  // `sources = []` is how a manifest turns test discovery off. Treating an
  // empty array as "unset" inverted the author's intent.
  const auto disabled =
      round_trip(std::string(kPackageHeader) + "[test]\nsources = []\n", temp.path());
  COVA_CHECK_MSG(disabled.test.sources.empty(), "an explicit empty array must stay empty");

  // Omitting the section still applies the documented default.
  const auto omitted = round_trip(std::string(kPackageHeader), temp.path());
  COVA_CHECK_EQ(omitted.test.sources.size(), std::size_t{1});
}

COVA_TEST_NAMED(manifest, optimization_is_range_checked_and_size_is_preserved) {
  cova::test::TempDir temp;
  auto too_large = cova::manifest::parse(std::string(kPackageHeader) +
                                             "[profile.dev]\noptimization = 99\n",
                                         "cova.toml", temp.path());
  COVA_CHECK_MSG(!too_large.has_value(), "optimization = 99 must be rejected");

  const auto sized =
      round_trip(std::string(kPackageHeader) + "[profile.release]\noptimization = \"s\"\n",
                 temp.path());
  const auto& release = sized.profiles.at("release");
  COVA_CHECK_MSG(release.optimize_for_size, "optimization = \"s\" must keep the size flag");
  bool has_os = false;
  for (const auto& flag : release.cxx_flags()) {
    if (flag == "-Os") {
      has_os = true;
    }
  }
  COVA_CHECK_MSG(has_os, "a size profile must emit -Os, not -O2");
}

COVA_TEST_NAMED(manifest, gnu_editions_report_their_real_standard) {
  cova::test::TempDir temp;
  auto manifest = cova::manifest::default_manifest("gnu", cova::manifest::PackageKind::Executable);
  // "gnu++17" was parsed as "++17", the exception was swallowed and the result
  // silently became the newest standard.
  manifest.package.edition = "gnu++17";
  COVA_CHECK_EQ(manifest.package.cpp_standard(), 17);
  manifest.package.edition = "gnu++20";
  COVA_CHECK_EQ(manifest.package.cpp_standard(), 20);
  manifest.package.edition = "c++23";
  COVA_CHECK_EQ(manifest.package.cpp_standard(), 23);
  // An unrecognised edition reports 0 so callers can reject it.
  manifest.package.edition = "c++99";
  COVA_CHECK_EQ(manifest.package.cpp_standard(), 0);
}

COVA_TEST_MAIN()
