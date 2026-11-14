// Robustness tests for the in-tree TOML and JSON parsers.
//
// These cover the failure modes a manifest or lockfile can actually hit:
// hostile nesting (stack exhaustion), malformed dates, invalid UTF-8 escapes
// and numeric edge cases. Each of them previously either crashed the process or
// silently accepted something a conforming reader rejects.
#include <string>

#include "core/json/json.hpp"
#include "core/toml/toml.hpp"
#include "testing/test.hpp"

// ---------------------------------------------------------------------------
// TOML: nesting depth
// ---------------------------------------------------------------------------

COVA_TEST_NAMED(toml, deep_nesting_is_rejected_instead_of_crashing) {
  // 20_000 nested arrays used to overflow the stack and kill the process
  // (SIGSEGV) instead of producing a parse error. Cova must report an error.
  constexpr int kDepth = 20000;
  std::string document = "a = ";
  document.append(kDepth, '[');
  document.append(kDepth, ']');

  auto parsed = cova::toml::parse(document, "deep.toml");
  COVA_CHECK_MSG(!parsed.has_value(), "deeply nested arrays must be rejected");
  if (!parsed) {
    COVA_CHECK_EQ(parsed.error().code, std::string("cova::toml::parse"));
    // The error must still carry a location so the user can find the value.
    COVA_CHECK(!parsed.error().context.empty());
  }
}

COVA_TEST_NAMED(toml, reasonable_nesting_still_parses) {
  auto parsed = cova::toml::parse("a = [[1, 2], [3, [4]]]", "ok.toml");
  COVA_CHECK(parsed.has_value());
}

COVA_TEST_NAMED(toml, depth_counter_recovers_between_values) {
  // A rejection must not poison the counter: a later sibling value has to parse
  // at its own depth.
  auto parsed = cova::toml::parse("a = [[1]]\nb = [[[2]]]", "siblings.toml");
  COVA_CHECK(parsed.has_value());
}

// ---------------------------------------------------------------------------
// TOML: dates
// ---------------------------------------------------------------------------

COVA_TEST_NAMED(toml, accepts_valid_date_and_time_forms) {
  for (const char* document :
       {"x = 1979-05-27T07:32:00Z", "x = 1979-05-27", "x = 07:32:00",
        "x = 1979-05-27T07:32:00.999-07:00", "x = 1979-05-27 07:32:00"}) {
    auto parsed = cova::toml::parse(document, "dates.toml");
    COVA_CHECK_MSG(parsed.has_value(), std::string("rejected a valid date: ") + document);
  }
}

COVA_TEST_NAMED(toml, rejects_out_of_range_and_trailing_datetime_junk) {
  // A typo'd date used to be stored verbatim and passed manifest validation.
  for (const char* document :
       {"x = 1979-05-27T99:99:99", "x = 1979-05-27T07:32:00ZZ", "x = 1979-13-01",
        "x = 1979-05-45", "x = 1979-05-27T07:32:00+99:99"}) {
    auto parsed = cova::toml::parse(document, "dates.toml");
    COVA_CHECK_MSG(!parsed.has_value(), std::string("accepted an invalid date: ") + document);
  }
}

// ---------------------------------------------------------------------------
// TOML: general structure
// ---------------------------------------------------------------------------

COVA_TEST_NAMED(toml, round_trips_scalars_and_tables) {
  auto parsed = COVA_REQUIRE_OK(cova::toml::parse(
      "name = \"cova\"\ncount = 3\nratio = 1.5\nflag = true\n[table]\nkey = \"v\"\n",
      "round.toml"));
  const std::string rendered = cova::toml::write(parsed);
  auto reparsed = cova::toml::parse(rendered, "round2.toml");
  COVA_CHECK(reparsed.has_value());
  if (reparsed) {
    COVA_CHECK_EQ(cova::toml::write(*reparsed), rendered);
  }
}

COVA_TEST_NAMED(toml, rejects_duplicate_keys_and_leading_zero_integers) {
  COVA_CHECK(!cova::toml::parse("a = 1\na = 2\n", "dup.toml").has_value());
  COVA_CHECK(!cova::toml::parse("a = 01\n", "zero.toml").has_value());
}

COVA_TEST_NAMED(toml, rejects_unterminated_values) {
  for (const char* document : {"a = [1, 2", "a = \"unterminated", "a = {b = 1"}) {
    COVA_CHECK_MSG(!cova::toml::parse(document, "broken.toml").has_value(),
                   std::string("accepted an unterminated value: ") + document);
  }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

COVA_TEST_NAMED(json, lone_surrogate_becomes_replacement_character) {
  // Encoding a lone UTF-16 surrogate produced CESU-8 bytes that are not valid
  // UTF-8, so the emitted document was rejected by strict readers.
  auto parsed = COVA_REQUIRE_OK(cova::json::Value::parse("{\"a\":\"\\uD800\"}"));
  const std::string dumped = parsed.dump(0);
  COVA_CHECK_MSG(dumped.find("\xEF\xBF\xBD") != std::string::npos,
                 "a lone surrogate must be replaced with U+FFFD");
}

COVA_TEST_NAMED(json, valid_surrogate_pair_decodes_to_one_code_point) {
  auto parsed = COVA_REQUIRE_OK(cova::json::Value::parse("{\"a\":\"\\uD83D\\uDE00\"}"));
  const std::string dumped = parsed.dump(0);
  COVA_CHECK_MSG(dumped.find("\xF0\x9F\x98\x80") != std::string::npos,
                 "a surrogate pair must decode to the astral code point");
}

COVA_TEST_NAMED(json, rejects_malformed_documents) {
  for (const char* document : {"{\"a\":}", "{\"a\":1}extra", "{\"a\":01}",
                               "{\"a\":\"raw\x01control\"}", "not json"}) {
    auto parsed = cova::json::Value::parse(document);
    COVA_CHECK_MSG(!parsed.has_value(), std::string("accepted malformed JSON: ") + document);
  }
}

COVA_TEST_NAMED(json, deeply_nested_input_is_rejected_not_fatal) {
  const std::string document(20000, '[');
  auto parsed = cova::json::Value::parse(document);
  COVA_CHECK(!parsed.has_value());
}

COVA_TEST_NAMED(json, unsigned_values_clamp_instead_of_wrapping) {
  // A byte count above INT64_MAX used to serialise as -1.
  const auto dumped = cova::json::Value(18446744073709551615ull).dump(0);
  COVA_CHECK_EQ(dumped, std::string("9223372036854775807"));
}

COVA_TEST_NAMED(json, as_int_returns_fallback_outside_the_int64_range) {
  COVA_CHECK_EQ(cova::json::Value(1e308).as_int(-7), std::int64_t{-7});
  COVA_CHECK_EQ(cova::json::Value(-1e308).as_int(-7), std::int64_t{-7});
  // In-range doubles still convert.
  COVA_CHECK_EQ(cova::json::Value(42.0).as_int(-7), std::int64_t{42});
}

COVA_TEST_NAMED(json, round_trips_nested_structures) {
  const std::string document = "{\"a\":1,\"b\":[1,2,{\"c\":null}],\"d\":true,\"e\":\"x\\ny\"}";
  auto parsed = COVA_REQUIRE_OK(cova::json::Value::parse(document));
  auto reparsed = COVA_REQUIRE_OK(cova::json::Value::parse(parsed.dump(0)));
  COVA_CHECK_EQ(reparsed.dump(0), parsed.dump(0));
}

COVA_TEST_MAIN()
