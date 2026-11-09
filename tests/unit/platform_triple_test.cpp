// Cova — target triple and ABI model tests (PRD §6).
#include <algorithm>
#include <string>

#include "platform/abi/abi.hpp"
#include "platform/constraints/constraints.hpp"
#include "platform/triple/triple.hpp"
#include "testing/test.hpp"

using cova::platform::Triple;

COVA_TEST_NAMED(platform, parses_canonical_triple) {
  auto triple = COVA_REQUIRE_OK(Triple::parse("x86_64-unknown-linux-gnu"));
  COVA_CHECK(triple.arch == cova::platform::Arch::X86_64);
  COVA_CHECK(triple.os == cova::platform::Os::Linux);
  COVA_CHECK(triple.env == cova::platform::Env::Gnu);
  COVA_CHECK_EQ(triple.to_string(), std::string("x86_64-unknown-linux-gnu"));
  COVA_CHECK_EQ(triple.to_short_string(), std::string("x86_64-linux-gnu"));
  COVA_CHECK(triple.abi() == cova::platform::Abi::SysV);
  COVA_CHECK(triple.crt() == cova::platform::Crt::Glibc);
  COVA_CHECK(triple.is_native_like() == (triple == cova::platform::host_triple()));
}

COVA_TEST_NAMED(platform, accepts_common_aliases) {
  auto amd64 = COVA_REQUIRE_OK(Triple::parse("amd64-unknown-linux-gnu"));
  COVA_CHECK(amd64.arch == cova::platform::Arch::X86_64);
  auto arm64 = COVA_REQUIRE_OK(Triple::parse("arm64-linux-musl"));
  COVA_CHECK(arm64.arch == cova::platform::Arch::Aarch64);
  COVA_CHECK(arm64.env == cova::platform::Env::Musl);
  auto mac = COVA_REQUIRE_OK(Triple::parse("aarch64-apple-darwin"));
  COVA_CHECK(mac.os == cova::platform::Os::Macos);
  COVA_CHECK(mac.vendor == cova::platform::Vendor::Apple);
}

COVA_TEST_NAMED(platform, accepts_mingw_triple_spellings) {
  auto mingw = COVA_REQUIRE_OK(Triple::parse("x86_64-w64-mingw32"));
  COVA_CHECK(mingw.arch == cova::platform::Arch::X86_64);
  COVA_CHECK(mingw.vendor == cova::platform::Vendor::Pc);
  COVA_CHECK(mingw.os == cova::platform::Os::Windows);
  COVA_CHECK(mingw.env == cova::platform::Env::Gnu);
  COVA_CHECK_EQ(mingw.to_string(), std::string("x86_64-pc-windows-gnu"));

  auto shortened = COVA_REQUIRE_OK(Triple::parse("x86_64-mingw64"));
  COVA_CHECK(shortened.os == cova::platform::Os::Windows);
  COVA_CHECK(shortened.env == cova::platform::Env::Gnu);

  // The user-facing spelling resolves to the canonical known target.
  auto resolved = COVA_REQUIRE_OK(cova::platform::resolve_target("x86_64-w64-mingw32"));
  COVA_CHECK_EQ(resolved.to_string(), std::string("x86_64-pc-windows-gnu"));
  COVA_CHECK(cova::platform::is_known_target("x86_64-w64-mingw32"));
}

COVA_TEST_NAMED(platform, normalises_pc_vendor_for_linux) {
  auto triple = COVA_REQUIRE_OK(Triple::parse("x86_64-pc-linux-gnu"));
  COVA_CHECK(triple.vendor == cova::platform::Vendor::UnknownVendor);
  COVA_CHECK_EQ(triple.to_string(), std::string("x86_64-unknown-linux-gnu"));
}

COVA_TEST_NAMED(platform, rejects_nonsense) {
  auto error = COVA_REQUIRE_ERR(Triple::parse("definitely-not-a-target"));
  COVA_CHECK(error.code.find("target") != std::string::npos);
}

COVA_TEST_NAMED(platform, mvp_targets_are_known) {
  COVA_CHECK(cova::platform::is_known_target("x86_64-unknown-linux-gnu"));
  COVA_CHECK(cova::platform::is_known_target("aarch64-unknown-linux-musl"));
  COVA_CHECK(!cova::platform::is_known_target("mips-unknown-none"));
  auto names = cova::platform::known_target_names();
  COVA_CHECK(std::is_sorted(names.begin(), names.end()));
}

COVA_TEST_NAMED(platform, parses_os_versions) {
  auto version = COVA_REQUIRE_OK(cova::platform::OsVersion::parse("11.2"));
  COVA_CHECK_EQ(version.major, 11);
  COVA_CHECK_EQ(version.minor, 2);
  COVA_CHECK_EQ(version.to_string(), std::string("11.2"));
  auto error = COVA_REQUIRE_ERR(cova::platform::OsVersion::parse("eleven"));
  COVA_CHECK(error.code.find("abi") != std::string::npos);
}

COVA_TEST_NAMED(platform, abi_signature_identity_is_stable) {
  cova::platform::AbiSignature signature;
  signature.triple = COVA_REQUIRE_OK(Triple::parse("x86_64-unknown-linux-gnu"));
  signature.abi = cova::platform::Abi::SysV;
  signature.crt = cova::platform::Crt::Glibc;
  signature.libcpp = cova::platform::LibCpp::LibStdCpp;
  signature.threads = cova::platform::ThreadModel::Posix;
  COVA_CHECK(!signature.fingerprint_key().empty());
  auto copy = signature;
  COVA_CHECK_EQ(copy.fingerprint_key(), signature.fingerprint_key());
  copy.exceptions = false;
  COVA_CHECK(copy.fingerprint_key() != signature.fingerprint_key());
  auto check = cova::platform::check_abi_compatibility(signature, signature);
  COVA_CHECK(check.verdict == cova::platform::AbiVerdict::Compatible);
}

COVA_TEST_NAMED(platform, constraints_evaluate) {
  cova::platform::CandidateEnvironment environment;
  environment.triple = COVA_REQUIRE_OK(Triple::parse("x86_64-unknown-linux-gnu"));
  environment.crt = cova::platform::Crt::Glibc;
  environment.libcpp = cova::platform::LibCpp::LibStdCpp;
  environment.abi = cova::platform::Abi::SysV;
  environment.threads = cova::platform::ThreadModel::Posix;

  cova::platform::Constraint crt_constraint;
  crt_constraint.kind = cova::platform::ConstraintKind::Crt;
  crt_constraint.key = "crt";
  crt_constraint.value = "glibc";
  auto verdict = cova::platform::evaluate(environment, {crt_constraint});
  COVA_CHECK(verdict.satisfied);

  crt_constraint.value = "musl";
  verdict = cova::platform::evaluate(environment, {crt_constraint});
  COVA_CHECK(!verdict.satisfied);
  COVA_CHECK(verdict.reason == cova::platform::RejectionReason::ConstraintUnsatisfied);
}

COVA_TEST_MAIN()
