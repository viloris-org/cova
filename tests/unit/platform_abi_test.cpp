// Cova — ABI identity, argument classification and evidence tests (FR-03/04/15).
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "platform/abi/abi.hpp"
#include "platform/triple/triple.hpp"
#include "testing/test.hpp"

namespace {

cova::platform::AbiSignature baseline() {
  cova::platform::AbiSignature signature;
  signature.triple = COVA_REQUIRE_OK(cova::platform::Triple::parse("x86_64-unknown-linux-gnu"));
  signature.abi = cova::platform::Abi::SysV;
  signature.crt = cova::platform::Crt::Glibc;
  signature.libcpp = cova::platform::LibCpp::LibStdCpp;
  signature.threads = cova::platform::ThreadModel::Posix;
  signature.exceptions_state = cova::platform::AbiFactState::Known;
  signature.rtti_state = cova::platform::AbiFactState::Known;
  return signature;
}

bool contains(const std::vector<std::string>& values, std::string_view needle) {
  return std::any_of(values.begin(), values.end(),
                     [needle](const std::string& value) { return value == needle; });
}

}  // namespace

COVA_TEST_NAMED(platform, abi_argument_classifier_separates_affecting_from_neutral) {
  using cova::platform::AbiArgumentClass;
  COVA_CHECK(cova::platform::classify_abi_argument("-O2").classification ==
             AbiArgumentClass::AbiNeutral);
  COVA_CHECK(cova::platform::classify_abi_argument("-g").classification ==
             AbiArgumentClass::AbiNeutral);
  COVA_CHECK(cova::platform::classify_abi_argument("-Wall").classification ==
             AbiArgumentClass::AbiNeutral);
  COVA_CHECK(cova::platform::classify_abi_argument("-I/usr/include").classification ==
             AbiArgumentClass::AbiNeutral);
  COVA_CHECK(cova::platform::classify_abi_argument("unit.cpp").classification ==
             AbiArgumentClass::AbiNeutral);

  COVA_CHECK(cova::platform::classify_abi_argument("-fno-exceptions").classification ==
             AbiArgumentClass::AbiAffecting);
  COVA_CHECK(cova::platform::classify_abi_argument("-fshort-enums").classification ==
             AbiArgumentClass::AbiAffecting);
  COVA_CHECK(cova::platform::classify_abi_argument("-mabi=lp64").classification ==
             AbiArgumentClass::AbiAffecting);
  COVA_CHECK(cova::platform::classify_abi_argument("-stdlib=libc++").classification ==
             AbiArgumentClass::AbiAffecting);
  COVA_CHECK(cova::platform::classify_abi_argument("-D_GLIBCXX_USE_CXX11_ABI=0").classification ==
             AbiArgumentClass::AbiAffecting);

  // Unknown switches are not silently neutral.
  COVA_CHECK(cova::platform::classify_abi_argument("-fmade-up-abi-flag").classification ==
             AbiArgumentClass::PotentiallyAbiAffecting);
  COVA_CHECK(cova::platform::classify_abi_argument("-DUNIT_MACRO=1").classification ==
             AbiArgumentClass::PotentiallyAbiAffecting);
}

COVA_TEST_NAMED(platform, effective_abi_resolves_ordered_overrides) {
  auto base = baseline();
  auto abi = cova::platform::effective_abi(base, {"-fno-exceptions", "-fexceptions"});
  COVA_CHECK(abi.signature.exceptions);
  COVA_CHECK(abi.signature.exceptions_state == cova::platform::AbiFactState::Known);
  COVA_CHECK(contains(abi.signature.abi_affecting_features, "exceptions=1"));

  auto reversed = cova::platform::effective_abi(base, {"-fexceptions", "-fno-exceptions"});
  COVA_CHECK(!reversed.signature.exceptions);
  COVA_CHECK(contains(reversed.signature.abi_affecting_features, "exceptions=0"));
}

COVA_TEST_NAMED(platform, effective_abi_resolves_last_wins_layout_flags) {
  auto base = baseline();
  auto short_off = cova::platform::effective_abi(base, {"-fshort-enums", "-fno-short-enums"});
  COVA_CHECK(contains(short_off.signature.abi_affecting_features, "short-enums=0"));
  COVA_CHECK(!contains(short_off.signature.abi_affecting_features, "short-enums=1"));

  auto packed = cova::platform::effective_abi(base, {"-fpack-struct=8", "-fno-pack-struct"});
  COVA_CHECK(contains(packed.signature.abi_affecting_features, "pack-struct=0"));
  COVA_CHECK(!contains(packed.signature.abi_affecting_features, "pack-struct=8"));

  auto abi = cova::platform::effective_abi(base, {"-mabi=lp64", "-mabi=ilp32"});
  COVA_CHECK(contains(abi.signature.abi_affecting_features, "mabi=ilp32"));
  COVA_CHECK(!contains(abi.signature.abi_affecting_features, "mabi=lp64"));

  auto stdlib =
      cova::platform::effective_abi(base, {"-stdlib=libc++", "-stdlib=libstdc++"});
  COVA_CHECK(stdlib.signature.libcpp == cova::platform::LibCpp::LibStdCpp);
  COVA_CHECK(contains(stdlib.signature.abi_affecting_features, "libcpp=libstdc++"));
}

COVA_TEST_NAMED(platform, effective_abi_does_not_pollute_with_neutral_flags) {
  auto base = baseline();
  base.abi_affecting_features.clear();
  const std::string baseline_key = base.fingerprint_key();
  auto neutral = cova::platform::effective_abi(base, {"-O2", "-g", "-Wall", "-I/usr/include"});
  COVA_CHECK_EQ(neutral.signature.fingerprint_key(), baseline_key);
  COVA_CHECK(neutral.state == cova::platform::AbiFactState::Known);

  auto unknown = cova::platform::effective_abi(base, {"-fmade-up-abi-flag"});
  COVA_CHECK(unknown.state == cova::platform::AbiFactState::Unknown);
  COVA_CHECK(neutral.identity_key() != unknown.identity_key());

  auto affecting = cova::platform::effective_abi(base, {"-fno-rtti"});
  COVA_CHECK(!affecting.signature.rtti);
  COVA_CHECK(neutral.identity_key() != affecting.identity_key());
}

COVA_TEST_NAMED(platform, abi_facts_are_applied_and_scope_tagged) {
  auto base = baseline();
  base.exceptions_state = cova::platform::AbiFactState::Unknown;
  base.rtti_state = cova::platform::AbiFactState::Unknown;
  std::map<std::string, std::string, std::less<>> facts;
  facts["__EXCEPTIONS"] = "1";
  facts["__GXX_RTTI"] = "0";
  facts["_GLIBCXX_USE_CXX11_ABI"] = "1";
  facts["__SIZEOF_POINTER__"] = "8";
  auto applied = cova::platform::apply_abi_facts(base, "gcc", facts);
  COVA_CHECK(applied.exceptions);
  COVA_CHECK(applied.exceptions_state == cova::platform::AbiFactState::Known);
  COVA_CHECK(!applied.rtti);
  COVA_CHECK(applied.rtti_state == cova::platform::AbiFactState::Known);
  COVA_CHECK(contains(applied.abi_affecting_features, "_GLIBCXX_USE_CXX11_ABI=1"));
  COVA_CHECK(contains(applied.abi_affecting_features,
                      "data-layout/__SIZEOF_POINTER__=8"));
  COVA_CHECK(!applied.facts.empty());
  for (const auto& fact : applied.facts) {
    COVA_CHECK(fact.scope == cova::platform::AbiEvidenceScope::Toolchain);
    COVA_CHECK(!fact.subject.empty());
    COVA_CHECK(!fact.source.empty());
  }
  // Missing facts stay Unknown rather than receiving a default.
  std::map<std::string, std::string, std::less<>> partial;
  partial["_GLIBCXX_USE_CXX11_ABI"] = "1";
  auto unknown = cova::platform::apply_abi_facts(base, "gcc", partial);
  COVA_CHECK(unknown.exceptions_state == cova::platform::AbiFactState::Unknown);
  COVA_CHECK(unknown.rtti_state == cova::platform::AbiFactState::Unknown);
}

COVA_TEST_NAMED(platform, abi_facts_fingerprint_is_order_independent) {
  cova::platform::AbiFact first;
  first.name = "__EXCEPTIONS";
  first.value = "1";
  first.state = cova::platform::AbiFactState::Known;
  first.subject = "toolchain";
  first.configuration = "default";
  first.source = "probe";
  first.producer_version = "2";
  cova::platform::AbiFact second = first;
  second.name = "_GLIBCXX_USE_CXX11_ABI";
  COVA_CHECK_EQ(cova::platform::abi_facts_fingerprint({first, second}),
                cova::platform::abi_facts_fingerprint({second, first}));
  second.value = "0";
  COVA_CHECK(cova::platform::abi_facts_fingerprint({first, second}) !=
             cova::platform::abi_facts_fingerprint({first, first}));
}

COVA_TEST_NAMED(platform, unknown_abi_facts_never_claim_compatibility) {
  auto signature = baseline();
  auto check = cova::platform::check_abi_compatibility(signature, signature);
  COVA_CHECK(check.verdict == cova::platform::AbiVerdict::Compatible);

  auto unknown = signature;
  unknown.exceptions_state = cova::platform::AbiFactState::Unknown;
  auto unproven = cova::platform::check_abi_compatibility(unknown, unknown);
  COVA_CHECK(unproven.verdict == cova::platform::AbiVerdict::Unknown);

  auto disabled = signature;
  disabled.exceptions = false;
  disabled.exceptions_state = cova::platform::AbiFactState::Known;
  auto incompatible = cova::platform::check_abi_compatibility(signature, disabled);
  COVA_CHECK(incompatible.verdict == cova::platform::AbiVerdict::Incompatible);
}

COVA_TEST_NAMED(platform, abi_signature_schema_is_part_of_the_identity) {
  auto signature = baseline();
  const std::string key = signature.fingerprint_key();
  auto copy = signature;
  copy.exceptions_state = cova::platform::AbiFactState::Unknown;
  COVA_CHECK(key != copy.fingerprint_key());
}

COVA_TEST_MAIN()
