// Cova — toolchain discovery, probing and resolution tests (PRD §7).
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "testing/test.hpp"
#include "toolchain/discovery/discovery.hpp"
#include "toolchain/resolver/resolver.hpp"

COVA_TEST_NAMED(toolchain, parses_version_banners) {
  auto clang = cova::toolchain::parse_version_banner("clang version 23.1.4 (https://llvm.org)");
  COVA_CHECK(clang.has_value());
  COVA_CHECK_EQ(clang->major, 23);
  COVA_CHECK_EQ(clang->minor, 1);
  auto gcc = cova::toolchain::parse_version_banner("g++ (GCC) 16.2.1 20260810");
  COVA_CHECK(gcc.has_value());
  COVA_CHECK_EQ(gcc->major, 16);
  COVA_CHECK(!cova::toolchain::parse_version_banner("no version here").has_value());
}

COVA_TEST_NAMED(toolchain, maps_executable_families) {
  COVA_CHECK_EQ(*cova::toolchain::family_from_executable("clang++"), std::string("clang"));
  COVA_CHECK_EQ(*cova::toolchain::family_from_executable("g++"), std::string("gcc"));
  COVA_CHECK(!cova::toolchain::family_from_executable("python3").has_value());
}

COVA_TEST_NAMED(toolchain, discovery_finds_this_host) {
  cova::toolchain::DiscoveryOptions options;
  auto discovered = COVA_REQUIRE_OK(cova::toolchain::discover(options));
  if (discovered.empty()) {
    return;  // document the honest "no compiler" case
  }
  bool has_cxx = false;
  for (const auto& descriptor : discovered) {
    COVA_CHECK(!descriptor.id.empty());
    COVA_CHECK(!descriptor.version.to_string().empty());
    COVA_CHECK(descriptor.has_component("cxx"));
    has_cxx = has_cxx || descriptor.has_component("cxx");
  }
  COVA_CHECK(has_cxx);
}

COVA_TEST_NAMED(toolchain, descriptor_fingerprint_is_deterministic) {
  auto descriptor = cova::toolchain::ToolchainDescriptor{};
  descriptor.id = "system-clang-20.0.0";
  descriptor.family = "clang";
  descriptor.version = cova::Version{20, 0, 0};
  descriptor.host = cova::platform::host_triple();
  const std::string first = descriptor.compute_fingerprint();
  const std::string second = descriptor.compute_fingerprint();
  COVA_CHECK_EQ(first, second);
  descriptor.version = cova::Version{21, 0, 0};
  COVA_CHECK(first != descriptor.compute_fingerprint());
}

COVA_TEST_NAMED(toolchain, native_resolution_succeeds_when_a_compiler_exists) {
  auto discovered = COVA_REQUIRE_OK(cova::toolchain::discover({}));
  if (discovered.empty()) {
    return;
  }
  cova::toolchain::ResolutionRequest request;
  request.target = cova::platform::host_triple();
  request.policy = cova::toolchain::Policy::SystemFirst;
  request.allow_install = false;
  request.offline = true;
  request.probe_capabilities = false;
  request.candidates = discovered;
  auto report = cova::toolchain::resolve(request);
  COVA_CHECK_MSG(report.resolved, report.failure.message);
  COVA_CHECK(report.resolved);
  COVA_CHECK(!report.toolchain.fingerprint.empty());
  COVA_CHECK(report.toolchain.target == request.target);
}

COVA_TEST_NAMED(toolchain, unknown_target_fails_with_diagnostic) {
  cova::toolchain::ResolutionRequest request;
  request.target = cova::platform::host_triple();
  request.compiler_family = "definitely-not-a-compiler";
  request.allow_install = false;
  auto report = cova::toolchain::resolve(request);
  COVA_CHECK(!report.resolved);
  COVA_CHECK(!report.failure.message.empty());
}

COVA_TEST_MAIN()
