// Unit tests for cova::Version ordering and VersionRequirement matching.
//
// SemVer precedence (semver.org §11.4) is the contract here. Ordering is not a
// cosmetic detail: the toolchain resolver sorts candidates by version to pick
// the newest compiler, so a wrong comparison selects the wrong toolchain.
//
// The suite is deliberately table-driven and includes the cases that were
// previously wrong (`1.0.0-9` vs `1.0.0-10`, `^0.0.x`, `<=1.2`).
#include <algorithm>
#include <string>
#include <vector>

#include "core/version/version.hpp"
#include "testing/test.hpp"

namespace {

cova::Version parse_version(const char* text) {
  auto version = cova::Version::parse(text);
  COVA_CHECK_MSG(version.has_value(), std::string("version did not parse: ") + text);
  return *version;
}

bool satisfies(const char* requirement, const char* version) {
  auto parsed = cova::VersionRequirement::parse(requirement);
  COVA_CHECK_MSG(parsed.has_value(), std::string("requirement did not parse: ") + requirement);
  return parsed->matches(parse_version(version));
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

COVA_TEST_NAMED(version, parses_canonical_forms) {
  COVA_CHECK_EQ(parse_version("1.2.3").to_string(), std::string("1.2.3"));
  COVA_CHECK_EQ(parse_version("1.0.0-alpha.1").to_string(), std::string("1.0.0-alpha.1"));
  COVA_CHECK_EQ(parse_version("1.0.0+build.5").to_string(), std::string("1.0.0"));
  // A missing patch is tolerated and normalised to zero.
  COVA_CHECK_EQ(parse_version("1.2").to_string(), std::string("1.2.0"));
}

COVA_TEST_NAMED(version, rejects_malformed_input) {
  for (const char* text : {"", "1.0.0.0", "a.b.c", "1.0.0-"}) {
    auto parsed = cova::Version::parse(text);
    COVA_CHECK_MSG(!parsed.has_value(), std::string("accepted malformed version: ") + text);
    if (!parsed) {
      COVA_CHECK_EQ(parsed.error().code, std::string("cova::version::parse"));
    }
  }
}

// ---------------------------------------------------------------------------
// Ordering (SemVer §11.4)
// ---------------------------------------------------------------------------

COVA_TEST_NAMED(version, release_outranks_its_prereleases) {
  // A defaulted `operator<=>` compared `pre` as a plain string and got this
  // backwards, which made the resolver prefer `1.0.0-alpha` over `1.0.0`.
  COVA_CHECK(parse_version("1.0.0-alpha") < parse_version("1.0.0"));
  COVA_CHECK(parse_version("1.0.0-rc.1") < parse_version("1.0.0"));
  COVA_CHECK(!(parse_version("1.0.0") < parse_version("1.0.0-rc.1")));
}

COVA_TEST_NAMED(version, prerelease_identifiers_compare_numerically) {
  // Numeric identifiers must compare by value, not byte by byte.
  COVA_CHECK(parse_version("1.0.0-9") < parse_version("1.0.0-10"));
  COVA_CHECK(parse_version("1.0.0-beta.2") < parse_version("1.0.0-beta.11"));
  // Leading zeros compare numerically.
  COVA_CHECK(parse_version("1.0.0-007") == parse_version("1.0.0-7"));
}

COVA_TEST_NAMED(version, numeric_identifiers_sort_below_alphanumeric) {
  COVA_CHECK(parse_version("1.0.0-1") < parse_version("1.0.0-alpha"));
  COVA_CHECK(parse_version("1.0.0-alpha.1") < parse_version("1.0.0-alpha.beta"));
}

COVA_TEST_NAMED(version, follows_the_semver_precedence_chain) {
  // The exact chain from semver.org §11.4.
  const std::vector<const char*> chain = {
      "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
      "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",      "1.0.0",
  };
  for (std::size_t index = 0; index + 1 < chain.size(); ++index) {
    COVA_CHECK_MSG(parse_version(chain[index]) < parse_version(chain[index + 1]),
                   std::string("expected ") + chain[index] + " < " + chain[index + 1]);
  }
}

COVA_TEST_NAMED(version, resolver_sort_picks_the_newest) {
  // Mirrors the resolver's `return b.version < a.version;` "newest first" sort.
  std::vector<cova::Version> candidates = {parse_version("16.2.1"), parse_version("16.10.0"),
                                           parse_version("9.4.0")};
  std::sort(candidates.begin(), candidates.end(),
            [](const cova::Version& a, const cova::Version& b) { return b < a; });
  COVA_CHECK_EQ(candidates.front().to_string(), std::string("16.10.0"));
  COVA_CHECK_EQ(candidates.back().to_string(), std::string("9.4.0"));
}

// ---------------------------------------------------------------------------
// Requirements
// ---------------------------------------------------------------------------

COVA_TEST_NAMED(version, caret_pins_the_leftmost_nonzero_component) {
  // ^0.0.x must not accept later patches; that was the old behaviour and it
  // let a dependency resolve to an incompatible version.
  COVA_CHECK(satisfies("^0.0.3", "0.0.3"));
  COVA_CHECK(!satisfies("^0.0.3", "0.0.4"));
  COVA_CHECK(!satisfies("^0.0.3", "0.1.0"));

  COVA_CHECK(satisfies("^0.1.2", "0.1.9"));
  COVA_CHECK(!satisfies("^0.1.2", "0.2.0"));
  COVA_CHECK(!satisfies("^0.1.2", "0.9.0"));

  COVA_CHECK(satisfies("^0.2.3", "0.2.9"));
  COVA_CHECK(!satisfies("^0.2.3", "0.3.0"));

  COVA_CHECK(satisfies("^1.2.3", "1.9.0"));
  COVA_CHECK(!satisfies("^1.2.3", "2.0.0"));
}

COVA_TEST_NAMED(version, tilde_pins_the_minor_series) {
  COVA_CHECK(satisfies("~1.2.3", "1.2.9"));
  COVA_CHECK(!satisfies("~1.2.3", "1.3.0"));
  COVA_CHECK(satisfies("~1.2", "1.2.9"));
  COVA_CHECK(!satisfies("~1.2", "1.9.0"));
}

COVA_TEST_NAMED(version, partial_comparison_operators_cover_their_series) {
  // `<=1.2` means "the whole 1.2 series or older". Expanding it to `<=1.2.0`
  // rejected 1.2.5, which surprised users and mis-selected toolchains.
  COVA_CHECK(satisfies("<=1.2", "1.2.5"));
  COVA_CHECK(!satisfies("<=1.2", "1.3.0"));

  COVA_CHECK(!satisfies(">1.2", "1.2.9"));
  COVA_CHECK(satisfies(">1.2", "1.3.0"));

  // Fully specified versions keep exact semantics.
  COVA_CHECK(satisfies("<=1.2.3", "1.2.3"));
  COVA_CHECK(!satisfies("<=1.2.3", "1.2.4"));
  COVA_CHECK(satisfies(">1.2.3", "1.2.4"));
}

COVA_TEST_NAMED(version, bare_requirements_keep_their_documented_meaning) {
  COVA_CHECK(satisfies("1.2", "1.2.9"));
  COVA_CHECK(!satisfies("1.2", "1.9.0"));
  COVA_CHECK(satisfies("11", "11.4.0"));
  COVA_CHECK(!satisfies("11", "12.0.0"));
  COVA_CHECK(satisfies("*", "0.0.1"));
  // A fully specified bare requirement is an exact pin.
  COVA_CHECK(satisfies("1.2.3", "1.2.3"));
  COVA_CHECK(!satisfies("1.2.3", "1.2.4"));
}

COVA_TEST_NAMED(version, conjunctions_apply_every_bound) {
  COVA_CHECK(satisfies(">=1.0.0, <2.0.0", "1.5.0"));
  COVA_CHECK(!satisfies(">=1.0.0, <2.0.0", "2.0.0"));
  COVA_CHECK(!satisfies(">=1.0.0, <2.0.0", "0.9.0"));
}

COVA_TEST_NAMED(version, rejects_unsupported_requirements) {
  // `!=` and wildcard sections are not part of the documented grammar; they
  // must be rejected rather than silently treated as a match.
  for (const char* text : {"!=1.0.0", "1.x"}) {
    auto parsed = cova::VersionRequirement::parse(text);
    COVA_CHECK_MSG(!parsed.has_value(), std::string("accepted unsupported requirement: ") + text);
  }
}

COVA_TEST_NAMED(version, huge_prerelease_identifiers_do_not_overflow) {
  // Regression guard: parsing and comparing extreme identifiers must not throw
  // or wrap around.
  auto requirement = COVA_REQUIRE_OK(
      cova::VersionRequirement::parse(">=1.0.0-1"));
  const std::string huge(400, '9');
  auto candidate = COVA_REQUIRE_OK(cova::Version::parse("1.0.0-" + huge));
  COVA_CHECK(requirement.matches(candidate));
}

COVA_TEST_MAIN()
