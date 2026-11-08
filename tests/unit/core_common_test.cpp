// Unit tests for cova:: common vocabulary types and string helpers.
#include <cstdint>
#include <string>
#include <vector>

#include "core/common.hpp"
#include "core/version/version.hpp"
#include "testing/test.hpp"

COVA_TEST_NAMED(common, error_to_string_includes_code_and_context) {
  cova::Error error = cova::Error::make("cova::test::code", "something failed");
  error.with("expected: 1").with("found: 2");
  COVA_CHECK_EQ(error.code, std::string("cova::test::code"));
  const std::string rendered = error.to_string();
  COVA_CHECK(rendered.find("cova::test::code") != std::string::npos);
  COVA_CHECK(rendered.find("something failed") != std::string::npos);
  COVA_CHECK(rendered.find("expected: 1") != std::string::npos);
  COVA_CHECK(rendered.find("found: 2") != std::string::npos);
}

COVA_TEST_NAMED(common, error_with_chains) {
  cova::Error error = cova::Error::make("cova::test::code", "msg");
  cova::Error& chained = error.with("one").with("two");
  COVA_CHECK_EQ(&chained, &error);
  COVA_CHECK_EQ(error.context.size(), std::size_t{2});
}

COVA_TEST_NAMED(common, error_equality) {
  const cova::Error a = cova::Error::make("cova::test::code", "msg");
  const cova::Error b = cova::Error::make("cova::test::code", "msg");
  const cova::Error c = cova::Error::make("cova::test::other", "msg");
  COVA_CHECK(a == b);
  COVA_CHECK(!(a == c));
}

COVA_TEST_NAMED(common, result_and_status_helpers) {
  cova::Result<int> good = 42;
  COVA_CHECK(good.has_value());
  cova::Status status = cova::ok();
  COVA_CHECK(status.has_value());
  const cova::Status bad = cova::fail(cova::Error::make("cova::test::fail", "nope"));
  COVA_CHECK(!bad.has_value());
  COVA_CHECK_EQ(bad.error().code, std::string("cova::test::fail"));
  const cova::Result<int> failed = cova::fail<int>(cova::Error::make("cova::test::fail", "nope"));
  COVA_CHECK(!failed.has_value());
}

COVA_TEST_NAMED(common, trim) {
  COVA_CHECK_EQ(cova::str::trim("  hello  "), std::string("hello"));
  COVA_CHECK_EQ(cova::str::trim("\t\n\r hello \n\t "), std::string("hello"));
  COVA_CHECK_EQ(cova::str::trim(""), std::string(""));
  COVA_CHECK_EQ(cova::str::trim("   "), std::string(""));
  COVA_CHECK_EQ(cova::str::trim_view("  ab  "), std::string_view("ab"));
}

COVA_TEST_NAMED(common, case_conversion) {
  COVA_CHECK_EQ(cova::str::to_lower("HeLLo"), std::string("hello"));
  COVA_CHECK_EQ(cova::str::to_upper("HeLLo"), std::string("HELLO"));
  COVA_CHECK_EQ(cova::str::to_lower(""), std::string(""));
}

COVA_TEST_NAMED(common, predicates) {
  COVA_CHECK(cova::str::starts_with("cova.toml", "cova"));
  COVA_CHECK(!cova::str::starts_with("co", "cova"));
  COVA_CHECK(cova::str::ends_with("cova.toml", ".toml"));
  COVA_CHECK(!cova::str::ends_with("cova.toml", "cova"));
  COVA_CHECK(cova::str::iequals("Clang", "clang"));
  COVA_CHECK(!cova::str::iequals("clang", "gcc"));
  COVA_CHECK(cova::str::contains("aarch64-unknown-linux", "linux"));
  COVA_CHECK(!cova::str::contains("abc", "zzz"));
  COVA_CHECK(cova::str::contains("abc", ""));
}

COVA_TEST_NAMED(common, split_char) {
  const std::vector<std::string> parts = cova::str::split("a,b,,c", ',');
  COVA_CHECK_EQ(parts.size(), std::size_t{4});
  COVA_CHECK_EQ(parts[0], std::string("a"));
  COVA_CHECK_EQ(parts[2], std::string(""));
  COVA_CHECK_EQ(parts[3], std::string("c"));

  const std::vector<std::string> single = cova::str::split("alone", ',');
  COVA_CHECK_EQ(single.size(), std::size_t{1});
  COVA_CHECK_EQ(single[0], std::string("alone"));
}

COVA_TEST_NAMED(common, split_string) {
  const std::vector<std::string> parts = cova::str::split("x::y::z", "::");
  COVA_CHECK_EQ(parts.size(), std::size_t{3});
  COVA_CHECK_EQ(parts[1], std::string("y"));
}

COVA_TEST_NAMED(common, split_whitespace_quoted) {
  const std::vector<std::string> parts =
      cova::str::split_whitespace_quoted(R"(clang++ -I"a b" -DNAME='x y' main.cpp)");
  COVA_CHECK_EQ(parts.size(), std::size_t{4});
  COVA_CHECK_EQ(parts[0], std::string("clang++"));
  COVA_CHECK_EQ(parts[1], std::string("-Ia b"));
  COVA_CHECK_EQ(parts[2], std::string("-DNAME=x y"));
  COVA_CHECK_EQ(parts[3], std::string("main.cpp"));

  const std::vector<std::string> empty = cova::str::split_whitespace_quoted("   ");
  COVA_CHECK(empty.empty());
}

COVA_TEST_NAMED(common, join) {
  const std::vector<std::string> parts{"a", "b", "c"};
  COVA_CHECK_EQ(cova::str::join(std::span<const std::string>(parts), ", "), std::string("a, b, c"));
  const std::vector<std::string> single{"a"};
  COVA_CHECK_EQ(cova::str::join(std::span<const std::string>(single), ", "), std::string("a"));
  const std::vector<std::string> none;
  COVA_CHECK_EQ(cova::str::join(std::span<const std::string>(none), ", "), std::string(""));
}

COVA_TEST_NAMED(common, replace_all) {
  COVA_CHECK_EQ(cova::str::replace_all("a.b.c", ".", "/"), std::string("a/b/c"));
  COVA_CHECK_EQ(cova::str::replace_all("aaa", "aa", "b"), std::string("ba"));
  COVA_CHECK_EQ(cova::str::replace_all("abc", "z", "y"), std::string("abc"));
  COVA_CHECK_EQ(cova::str::replace_all("abc", "", "y"), std::string("abc"));
}

COVA_TEST_NAMED(common, identifiers) {
  COVA_CHECK(cova::str::is_identifier("hello"));
  COVA_CHECK(cova::str::is_identifier("_hello"));
  COVA_CHECK(cova::str::is_identifier("h1-_2"));
  COVA_CHECK(!cova::str::is_identifier(""));
  COVA_CHECK(!cova::str::is_identifier("1hello"));
  COVA_CHECK(!cova::str::is_identifier("hello world"));
  COVA_CHECK(!cova::str::is_identifier("hello.world"));
}

COVA_TEST_NAMED(common, package_names) {
  COVA_CHECK(cova::str::is_package_name("hello"));
  COVA_CHECK(cova::str::is_package_name("my-lib_2"));
  COVA_CHECK(!cova::str::is_package_name(""));
  COVA_CHECK(!cova::str::is_package_name("1lib"));
  COVA_CHECK(!cova::str::is_package_name("Lib"));
  COVA_CHECK(!cova::str::is_package_name("my.lib"));
}

COVA_TEST_NAMED(common, quote) {
  COVA_CHECK_EQ(cova::str::quote("plain"), std::string("\"plain\""));
  COVA_CHECK_EQ(cova::str::quote("a\"b"), std::string("\"a\\\"b\""));
  COVA_CHECK_EQ(cova::str::quote("a\\b"), std::string("\"a\\\\b\""));
}

COVA_TEST_NAMED(common, collapse_whitespace) {
  COVA_CHECK_EQ(cova::str::collapse_whitespace("  a \n\t b   c  "), std::string("a b c"));
  COVA_CHECK_EQ(cova::str::collapse_whitespace("   "), std::string(""));
}

COVA_TEST_NAMED(common, clamp_value) {
  COVA_CHECK_EQ(cova::clamp_value(5, 1, 10), 5);
  COVA_CHECK_EQ(cova::clamp_value(-5, 1, 10), 1);
  COVA_CHECK_EQ(cova::clamp_value(50, 1, 10), 10);
}

COVA_TEST_NAMED(common, fnv1a64_is_stable) {
  // Known FNV-1a 64-bit values; changing these would silently invalidate cache
  // identifiers that use fnv1a64.
  COVA_CHECK_EQ(cova::fnv1a64(""), std::uint64_t{14695981039346656037ull});
  COVA_CHECK_EQ(cova::fnv1a64("a"), std::uint64_t{12638187200555641996ull});
  COVA_CHECK(cova::fnv1a64("abc") != cova::fnv1a64("abd"));
  COVA_CHECK_EQ(cova::fnv1a64("repeat"), cova::fnv1a64("repeat"));
}

COVA_TEST_NAMED(common, human_bytes) {
  COVA_CHECK_EQ(cova::human_bytes(0), std::string("0 B"));
  COVA_CHECK_EQ(cova::human_bytes(512), std::string("512 B"));
  COVA_CHECK_EQ(cova::human_bytes(1024), std::string("1.0 KiB"));
  COVA_CHECK_EQ(cova::human_bytes(15ull * 1024 * 1024), std::string("15.0 MiB"));
  COVA_CHECK_EQ(cova::human_bytes(2ull * 1024 * 1024 * 1024), std::string("2.0 GiB"));
}

COVA_TEST_NAMED(common, version_extract_ignores_triple_components) {
  auto version =
      COVA_REQUIRE_OK(cova::Version::extract("x86_64-pc-linux-gnu-g++ (GCC) 16.2.1 20260810"));
  COVA_CHECK_EQ(version.to_string(), std::string("16.2.1"));
  auto clang = COVA_REQUIRE_OK(cova::Version::extract("clang version 23.1.4 (https://llvm.org)"));
  COVA_CHECK_EQ(clang.to_string(), std::string("23.1.4"));
}

COVA_TEST_MAIN()
