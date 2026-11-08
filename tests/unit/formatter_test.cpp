// Cova — formatter file selection and default style tests (PRD §11).
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "formatter/formatter.hpp"
#include "manifest/manifest.hpp"
#include "testing/test.hpp"

COVA_TEST_NAMED(formatter, recognises_formattable_extensions) {
  COVA_CHECK(cova::formatter::is_formattable("src/main.cpp"));
  COVA_CHECK(cova::formatter::is_formattable("include/a.hpp"));
  COVA_CHECK(cova::formatter::is_formattable("x.cc"));
  COVA_CHECK(!cova::formatter::is_formattable("README.md"));
  COVA_CHECK(!cova::formatter::is_formattable("cova.toml"));
}

COVA_TEST_NAMED(formatter, default_style_is_a_clang_format_file) {
  const std::string contents = cova::formatter::default_clang_format_contents();
  COVA_CHECK(contents.find("BasedOnStyle: LLVM") != std::string::npos);
  COVA_CHECK(contents.find("IndentWidth: 2") != std::string::npos);
}

COVA_TEST_NAMED(formatter, collects_package_and_test_sources) {
  cova::test::TempDir temp;
  const auto root = std::filesystem::path(temp.path()) / "project";
  COVA_CHECK(cova::fs::create_directories(root / "src").has_value());
  COVA_CHECK(cova::fs::create_directories(root / "tests").has_value());
  COVA_CHECK(cova::fs::write_file(root / "src" / "main.cpp", "int main() {}\n").has_value());
  COVA_CHECK(cova::fs::write_file(root / "tests" / "smoke.cpp", "int main() {}\n").has_value());
  COVA_CHECK(cova::fs::write_file(root / "README.md", "text\n").has_value());

  auto manifest = cova::manifest::default_manifest("project",
                                                   cova::manifest::PackageKind::Executable);
  manifest.path = root / "cova.toml";
  manifest.root = root;
  cova::formatter::FileSetOptions options;
  auto files = COVA_REQUIRE_OK(cova::formatter::collect_files(root, manifest, options));
  COVA_CHECK_EQ(files.size(), std::size_t{2});
}

COVA_TEST_NAMED(formatter, resolves_host_clang_format_when_present) {
  if (!cova::fs::find_in_path("clang-format").has_value()) {
    return;
  }
  cova::formatter::ResolveOptions options;
  options.allow_install = false;
  auto descriptor = COVA_REQUIRE_OK(cova::formatter::resolve_formatter(options));
  COVA_CHECK(descriptor.usable);
  COVA_CHECK(!descriptor.version_text.empty());
}

COVA_TEST_MAIN()
