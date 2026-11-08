// Cova — toolchain capability probing (PRD §7.6).
//
// Every probe compiles, archives or links a real input in a scratch directory.
// A probe is cached per descriptor fingerprint so `cova build` does not pay the
// cost on every invocation.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "toolchain/descriptor.hpp"

namespace cova::toolchain {

struct ProbeOptions {
  // Directory for scratch files; a temp dir is used when empty.
  std::filesystem::path work_dir;
  // Target the compiler will be invoked for.
  platform::Triple target;
  // Sysroot to pass to the compiler while probing. Empty means "use the
  // compiler's own configured sysroot". Cross probing must pass this, otherwise
  // a compiler that can emit target code may still be judged against host
  // headers and libraries.
  std::filesystem::path sysroot;
  // Capabilities the caller needs; probing may stop early once all are known.
  std::vector<std::string> required;  // "cpp", "c", "archive", "link", "stdlib", "c++26"
  int timeout_ms = 20000;
  bool keep_artifacts = false;
};

struct ProbeReport {
  ToolchainCapabilities capabilities;
  std::vector<std::string> notes;      // one line per probe performed
  std::vector<std::string> failures;   // probes that failed, with the reason
  bool succeeded = true;
};

// Runs the full capability matrix. Fills `capabilities` and `content_hash`.
Result<ProbeReport> probe(const ToolchainDescriptor& descriptor, const ProbeOptions& options);

// Individual probes, exposed for focused tests and for the doctor command.
Status probe_c_compilation(const ToolchainDescriptor& descriptor, const ProbeOptions& options);
Status probe_cpp_compilation(const ToolchainDescriptor& descriptor,
                             const ProbeOptions& options);
Status probe_static_library(const ToolchainDescriptor& descriptor,
                            const ProbeOptions& options);
Status probe_executable_link(const ToolchainDescriptor& descriptor,
                             const ProbeOptions& options);
Status probe_stdlib_headers(const ToolchainDescriptor& descriptor,
                            const ProbeOptions& options);
// Verifies the C++26 baseline features Cova requires for bootstrap (PRD §16.4):
// <expected>, <print>/<format>, concepts, ranges, designated initializers.
Status probe_cpp26_baseline(const ToolchainDescriptor& descriptor,
                            const ProbeOptions& options);

// Reads a cached probe report for this descriptor fingerprint. `variant` must
// include the probe target and sysroot, because the same compiler produces
// different answers for host and cross targets.
Result<ProbeReport> load_cached_probe(const std::filesystem::path& cache_dir,
                                      const ToolchainDescriptor& descriptor,
                                      std::string_view variant = {});
Status store_cached_probe(const std::filesystem::path& cache_dir,
                          const ToolchainDescriptor& descriptor, const ProbeReport& report,
                          std::string_view variant = {});

// Human-readable rows for `cova toolchain explain` / `cova doctor`.
std::string format_capability_table(const ToolchainCapabilities& capabilities);

}  // namespace cova::toolchain
