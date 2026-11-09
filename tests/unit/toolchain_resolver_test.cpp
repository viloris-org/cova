// Cova — toolchain discovery, probing and resolution tests (PRD §7).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/filesystem/filesystem.hpp"
#include "platform/triple/triple.hpp"
#include "testing/test.hpp"
#include "toolchain/discovery/discovery.hpp"
#include "toolchain/probe/probe.hpp"
#include "toolchain/resolver/resolver.hpp"

namespace {

void write_cross_driver_script(const std::filesystem::path& path) {
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

// A Linux target that is not this host, so "cross" stays cross on both
// x86_64 and aarch64 runners.
std::string non_host_linux_target() {
  return cova::platform::host_triple().arch == cova::platform::Arch::Aarch64
             ? "x86_64-unknown-linux-gnu"
             : "aarch64-unknown-linux-gnu";
}

std::string non_host_driver_prefix() {
  return cova::platform::host_triple().arch == cova::platform::Arch::Aarch64
             ? "x86_64-linux-gnu"
             : "aarch64-linux-gnu";
}

cova::toolchain::ToolchainDescriptor
fake_descriptor(const std::string& family, const std::string& target_text, bool self_contained) {
  const auto target = *cova::platform::Triple::parse(target_text);
  cova::toolchain::ToolchainDescriptor descriptor;
  descriptor.id = "test-" + family + "-" + target.to_short_string();
  descriptor.family = family;
  descriptor.version = cova::Version{14, 2, 0};
  descriptor.source = cova::toolchain::ToolchainSource::System;
  descriptor.host = cova::platform::host_triple();
  descriptor.supported_targets = {target.to_string(), target.to_short_string()};
  descriptor.crt = target.crt();
  descriptor.libcpp = target.default_libcpp();
  descriptor.abi = target.abi();
  descriptor.self_contained = self_contained;
  cova::toolchain::ToolComponent cxx;
  cxx.role = "cxx";
  cxx.path = "/bin/true";
  descriptor.components.push_back(std::move(cxx));
  descriptor.content_hash = "fake-content-hash";
  return descriptor;
}

} // namespace

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
    return; // document the honest "no compiler" case
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

COVA_TEST_NAMED(toolchain, recognises_cross_and_versioned_driver_names) {
  COVA_CHECK_EQ(*cova::toolchain::family_from_executable("aarch64-linux-gnu-g++"),
                std::string("gcc"));
  COVA_CHECK_EQ(*cova::toolchain::family_from_executable("aarch64-linux-musl-g++-12"),
                std::string("gcc"));
  COVA_CHECK_EQ(*cova::toolchain::family_from_executable("clang++-18"), std::string("clang"));
  COVA_CHECK_EQ(*cova::toolchain::family_from_executable("g++-14"), std::string("gcc"));

  auto target = cova::toolchain::target_from_executable("aarch64-linux-musl-g++");
  COVA_CHECK(target.has_value());
  COVA_CHECK_EQ(target->to_string(), std::string("aarch64-unknown-linux-musl"));
  COVA_CHECK(!cova::toolchain::target_from_executable("g++-14").has_value());
  COVA_CHECK(!cova::toolchain::target_from_executable("clang++").has_value());

  // MinGW driver prefixes use its own spelling; they still mean Windows GNU.
  auto mingw = cova::toolchain::target_from_executable("x86_64-w64-mingw32-g++");
  COVA_CHECK(mingw.has_value());
  COVA_CHECK_EQ(mingw->to_string(), std::string("x86_64-pc-windows-gnu"));
  auto mingw32 = cova::toolchain::target_from_executable("i686-w64-mingw32-g++");
  COVA_CHECK(mingw32.has_value());
  COVA_CHECK(mingw32->arch == cova::platform::Arch::X86);
  COVA_CHECK(mingw32->os == cova::platform::Os::Windows);
  COVA_CHECK(mingw32->env == cova::platform::Env::Gnu);
}

COVA_TEST_NAMED(toolchain, discovery_recognises_prefixed_cross_driver) {
#ifdef _WIN32
  return;  // the fixture driver is a POSIX shell script
#endif
  cova::test::TempDir temp;
  const auto bin = std::filesystem::path(temp.path()) / "bin";
  write_cross_driver_script(bin / (non_host_driver_prefix() + "-g++"));

  cova::toolchain::DiscoveryOptions options;
  options.include_system = false;
  options.include_managed = false;
  options.include_user_path = true;
  options.extra_roots = {bin};
  options.families = {"gcc"};
  auto discovered = COVA_REQUIRE_OK(cova::toolchain::discover(options));
  COVA_CHECK_EQ(discovered.size(), std::size_t{1});
  if (discovered.size() != 1) {
    return;
  }
  COVA_CHECK_EQ(discovered.front().family, std::string("gcc"));
  COVA_CHECK(discovered.front().self_contained);
  const auto& supported = discovered.front().supported_targets;
  COVA_CHECK(std::find(supported.begin(), supported.end(), non_host_linux_target()) !=
             supported.end());
}

COVA_TEST_NAMED(toolchain, discovery_recognises_mingw_cross_driver) {
#ifdef _WIN32
  return; // the fixture driver is a POSIX shell script
#endif
  cova::test::TempDir temp;
  const auto bin = std::filesystem::path(temp.path()) / "bin";
  write_cross_driver_script(bin / "x86_64-w64-mingw32-g++");

  cova::toolchain::DiscoveryOptions options;
  options.include_system = false;
  options.include_managed = false;
  options.include_user_path = true;
  options.extra_roots = {bin};
  options.families = {"gcc"};
  auto discovered = COVA_REQUIRE_OK(cova::toolchain::discover(options));
  COVA_CHECK_EQ(discovered.size(), std::size_t{1});
  if (discovered.size() != 1) {
    return;
  }
  const auto& descriptor = discovered.front();
  const auto& supported = descriptor.supported_targets;
  COVA_CHECK(std::find(supported.begin(), supported.end(),
                       "x86_64-pc-windows-gnu") != supported.end());
  COVA_CHECK(descriptor.self_contained); // MinGW carries its own CRT and libstdc++
}

COVA_TEST_NAMED(toolchain, host_gcc_cannot_satisfy_a_cross_target) {
  cova::toolchain::ResolutionRequest request;
  request.target = *cova::platform::Triple::parse(non_host_linux_target());
  request.allow_install = false;
  request.offline = true;
  request.probe_capabilities = false;
  request.candidates = {fake_descriptor("gcc", cova::platform::host_triple().to_string(), false)};
  auto report = cova::toolchain::resolve(request);
  COVA_CHECK(!report.resolved);
  COVA_CHECK(!report.rejected.empty());
  if (!report.rejected.empty()) {
    COVA_CHECK(report.rejected.front().reason ==
               cova::platform::RejectionReason::TargetUnsupported);
  }
}

COVA_TEST_NAMED(toolchain, cross_clang_uses_a_system_sysroot) {
  cova::test::TempDir temp;
  const auto root = std::filesystem::path(temp.path()) / "sysroot";
  COVA_CHECK(cova::fs::create_directories(root / "usr" / "include").has_value());
  COVA_CHECK(cova::fs::create_directories(root / "usr" / "lib").has_value());
  cova::test::ScopedEnv sysroot_env("COVA_SYSROOT", root.string());

  cova::toolchain::ResolutionRequest request;
  request.target = *cova::platform::Triple::parse(non_host_linux_target());
  request.allow_install = false;
  request.offline = true;
  request.probe_capabilities = false;
  request.candidates = {fake_descriptor("clang", request.target.to_string(),
                                        /*self_contained=*/false)};
  auto report = cova::toolchain::resolve(request);
  COVA_CHECK_MSG(report.resolved, report.failure.message);
  if (report.resolved) {
    COVA_CHECK_EQ(report.toolchain.sysroot_path, root);
    COVA_CHECK(report.toolchain.sysroot_id.find("system-") == 0);
  }
}

COVA_TEST_NAMED(toolchain, self_contained_cross_gcc_needs_no_sysroot) {
  cova::toolchain::ResolutionRequest request;
  request.target = *cova::platform::Triple::parse("aarch64-unknown-linux-musl");
  request.allow_install = false;
  request.offline = true;
  request.probe_capabilities = false;
  request.candidates = {fake_descriptor("gcc", request.target.to_string(),
                                        /*self_contained=*/true)};
  auto report = cova::toolchain::resolve(request);
  COVA_CHECK_MSG(report.resolved, report.failure.message);
  if (report.resolved) {
    COVA_CHECK(report.toolchain.sysroot_path.empty());
  }
}

COVA_TEST_NAMED(toolchain, offline_cross_without_sysroot_names_the_restriction) {
  cova::toolchain::ResolutionRequest request;
  request.target = *cova::platform::Triple::parse("aarch64-unknown-linux-musl");
  request.allow_install = false;
  request.offline = true;
  request.probe_capabilities = false;
  request.candidates = {fake_descriptor("clang", request.target.to_string(),
                                        /*self_contained=*/false)};
  auto report = cova::toolchain::resolve(request);
  COVA_CHECK(!report.resolved);
  COVA_CHECK(!report.rejected.empty());
  if (!report.rejected.empty()) {
    COVA_CHECK(report.rejected.front().reason ==
               cova::platform::RejectionReason::OfflineRestriction);
  }
}

COVA_TEST_NAMED(toolchain, probe_cache_is_versioned_and_rejects_mismatch) {
  cova::test::TempDir temp;
  const auto cache = std::filesystem::path(temp.path()) / "cache";
  cova::toolchain::ProbeOptions options;
  options.target = cova::platform::host_triple();
  options.environment_identity = "env-one";
  options.required = {"cpp", "link"};

  cova::toolchain::ToolchainDescriptor descriptor;
  descriptor.id = "dummy-gcc";
  descriptor.family = "gcc";
  descriptor.version = cova::Version{1, 2, 3};
  descriptor.content_hash = "content-hash";

  cova::toolchain::ProbeReport report;
  report.capabilities.cpp_compilation = true;
  report.abi_facts["_GLIBCXX_USE_CXX11_ABI"] = "1";
  report.notes.push_back("a probe note");
  COVA_CHECK(cova::toolchain::store_cached_probe(cache, descriptor, options, report, "variant")
                 .has_value());

  auto loaded = COVA_REQUIRE_OK(
      cova::toolchain::load_cached_probe(cache, descriptor, options, "variant"));
  COVA_CHECK(loaded.capabilities.cpp_compilation);
  COVA_CHECK_EQ(loaded.abi_facts.at("_GLIBCXX_USE_CXX11_ABI"), std::string("1"));
  COVA_CHECK_EQ(loaded.notes.size(), std::size_t{1});

  // A different effective environment is a different probe identity.
  options.environment_identity = "env-two";
  auto changed = cova::toolchain::load_cached_probe(cache, descriptor, options, "variant");
  COVA_CHECK(!changed.has_value());
  COVA_CHECK_EQ(changed.error().code, std::string("cova::toolchain::probe_cache_miss"));

  // An older schema on disk is a miss, never silently compatible.
  options.environment_identity = "env-one";
  for (const auto& entry : std::filesystem::directory_iterator(cache / "probes")) {
    auto text = COVA_REQUIRE_OK(cova::fs::read_file(entry.path()));
    text = cova::str::replace_all(text, "cova.probe/2", "cova.probe/1");
    COVA_CHECK(cova::fs::write_file(entry.path(), text).has_value());
    break;
  }
  auto old_schema = cova::toolchain::load_cached_probe(cache, descriptor, options, "variant");
  COVA_CHECK(!old_schema.has_value());
  COVA_CHECK_EQ(old_schema.error().code, std::string("cova::toolchain::probe_cache_miss"));
}

COVA_TEST_NAMED(toolchain, abi_probe_extracts_core_macros) {
  auto discovered = COVA_REQUIRE_OK(cova::toolchain::discover({}));
  if (discovered.empty()) {
    return;
  }
  cova::toolchain::ProbeOptions options;
  options.target = cova::platform::host_triple();
  const auto& descriptor = discovered.front();
  auto facts = cova::toolchain::probe_abi_facts(descriptor, options);
  if (!facts) {
    // A compiler that cannot dump macros leaves facts Unknown; that is a
    // supported state, not a test failure on exotic hosts.
    return;
  }
  const bool gcc_or_clang = descriptor.family.find("gcc") != std::string::npos ||
                            descriptor.family.find("clang") != std::string::npos;
  if (gcc_or_clang) {
    COVA_CHECK_MSG(facts->count("__EXCEPTIONS") == 1, "__EXCEPTIONS was not observed");
    COVA_CHECK_MSG(facts->count("__SIZEOF_POINTER__") == 1,
                   "__SIZEOF_POINTER__ was not observed");
  }
}

COVA_TEST_NAMED(toolchain, descriptor_fingerprint_covers_compiler_subprograms) {
  auto discovered = COVA_REQUIRE_OK(cova::toolchain::discover({}));
  if (discovered.empty()) {
    return;
  }
  bool found_subprogram = false;
  auto descriptor = discovered.front();
  cova::toolchain::enrich_compiler_subprograms(descriptor);
  for (const std::string_view role :
       {"cc1", "cc1plus", "assembler", "collect2", "lto-wrapper", "specs"}) {
    const auto* component = descriptor.component(role);
    if (component == nullptr) {
      continue;
    }
    found_subprogram = true;
    COVA_CHECK_MSG(!component->sha256.empty(), std::string(role));
    COVA_CHECK_MSG(!component->path.empty(), std::string(role));
  }
  const bool gcc_or_clang = descriptor.family.find("gcc") != std::string::npos ||
                            descriptor.family.find("clang") != std::string::npos;
  if (gcc_or_clang) {
    COVA_CHECK_MSG(found_subprogram,
                   "the GCC/Clang descriptor recorded no compiler subprogram");
  }
}

COVA_TEST_MAIN()
