// Cova — Toolchain Resolver (PRD §7).
//
// Resolution order (PRD §7.2):
//   discover → filter by hard constraints → rank by policy → verify components
//   → attach sysroot → freeze.
//
// The resolver never touches the build graph; the build layer only receives the
// frozen `ResolvedToolchain` (PRD §17.2).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "core/diagnostics/diagnostic.hpp"
#include "platform/constraints/constraints.hpp"
#include "platform/triple/triple.hpp"
#include "toolchain/descriptor.hpp"

namespace cova::toolchain {

struct ResolutionRequest {
  platform::Triple target;
  Policy policy = Policy::SystemFirst;
  // Requested compiler family and version requirement from [toolchain].
  std::optional<std::string> compiler_family;  // "clang", "gcc"
  std::optional<std::string> compiler_version; // ">=20, <23"
  // Hard constraints from the manifest and CLI.
  std::vector<platform::Constraint> constraints;
  // Pinned descriptor id (from cova-toolchain.lock); when set, only this
  // toolchain may satisfy the request (PRD §7.2, §15.2).
  std::optional<std::string> pinned_id;
  std::optional<std::string> pinned_content_hash;
  // Locked mode forbids selecting anything outside the lockfile.
  bool locked = false;
  // Installation behaviour.
  bool allow_install = true;
  bool offline = false;
  // Where managed components live.
  std::filesystem::path toolchains_root;
  std::filesystem::path sysroots_root;
  std::filesystem::path store_root;
  std::filesystem::path cache_root;
  // Pre-discovered candidates. When empty the resolver discovers its own.
  std::vector<ToolchainDescriptor> candidates;
  // Whether to run capability probes (tests may disable this).
  bool probe_capabilities = true;
  // Verbosity: collect a full decision trace.
  bool explain = false;
};

// One rejected candidate with the reason it was rejected (PRD §7.7).
struct RejectedCandidate {
  std::string toolchain_label;
  platform::RejectionReason reason = platform::RejectionReason::None;
  std::string explanation;
};

struct ResolutionReport {
  bool resolved = false;
  ResolvedToolchain toolchain;
  std::vector<RejectedCandidate> rejected;
  std::vector<std::string> decisions;
  // Diagnostic-ready error when resolution failed.
  Diagnostic failure;
};

// Resolves a complete compilation environment for the request. Returns a report
// rather than a Result so callers can render the full diagnostic (PRD §7.7).
ResolutionReport resolve(const ResolutionRequest& request);

// Ranks candidates for a policy without probing: lower is better.
struct RankedCandidate {
  ToolchainDescriptor descriptor;
  int rank = 0;
  std::string reason;
};
std::vector<RankedCandidate> rank_candidates(const std::vector<ToolchainDescriptor>& candidates,
                                             const ResolutionRequest& request);

// Attempt record: how far a candidate got before it was rejected.
struct CandidateAttempt {
  std::string label;
  bool matched_target = false;
  bool matched_version = false;
  bool matched_abi = false;
  bool probe_passed = false;
  bool sysroot_ok = false;
  std::vector<std::string> missing;
  platform::RejectionReason reason = platform::RejectionReason::None;
};

// Builds the "unable to resolve toolchain" diagnostic body from a request and
// the rejected candidates.
Diagnostic build_failure_diagnostic(const ResolutionRequest& request,
                                    const std::vector<CandidateAttempt>& attempts);

// Serialises a resolution result into cova-toolchain.lock form (PRD §15.1).
std::string toolchain_lock_entry(const ResolvedToolchain& resolved);

}  // namespace cova::toolchain
