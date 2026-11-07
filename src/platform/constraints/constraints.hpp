// Cova — target constraints and compatibility judgement (PRD §6.3, TGT-003/005).
//
// Constraints are matched against a *candidate environment* (a toolchain plus an
// optional sysroot). Every rejection carries a machine-readable reason so the
// resolver can build the "Required / Found / Missing" report from PRD §7.7.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "core/version/version.hpp"
#include "platform/abi/abi.hpp"
#include "platform/triple/triple.hpp"

namespace cova::platform {

enum class ConstraintKind {
  Arch,
  OsVersion,
  Crt,
  LibCpp,
  CpuMicroArch,
  CpuFeature,
  LinkMode,
  Exceptions,
  Rtti,
  ThreadModel,
  SdkVersion,
  Abi,
  Unknown,
};

std::string_view to_string(ConstraintKind kind);

// A single declarative requirement.
struct Constraint {
  ConstraintKind kind = ConstraintKind::Unknown;
  std::string key;    // e.g. "cpu-feature"
  std::string value;  // literal text, e.g. "sse4.2", "posix", "static"
  bool required = true;   // false = preference (soft), used for ranking only
  std::string origin;     // where it came from: "manifest", "cli", "target spec"

  std::string to_string() const;
};

// Parses constraints from `[build]`/`[target]` manifest tables and CLI flags.
// Recognised keys in a TOML table:
//   cpu-arch, cpu, link, exceptions, rtti, threads, crt, libcpp, os-version,
//   sdk, abi, features = ["sse4.2", ...]
Result<std::vector<Constraint>> constraints_from_table(const void* toml_table,
                                                      std::string origin);

// The thing constraints are judged against. Filled by the toolchain probe and
// the sysroot manager; frozen before build execution (PRD §17.3).
struct CandidateEnvironment {
  Triple triple;
  std::string compiler_family;      // "clang", "gcc", "msvc"
  std::optional<Version> compiler_version;
  std::optional<OsVersion> os_version;
  Crt crt = Crt::Unknown;
  LibCpp libcpp = LibCpp::Unknown;
  Abi abi = Abi::Unknown;
  std::optional<Version> sdk_version;
  bool supports_exceptions = true;
  bool supports_rtti = true;
  ThreadModel threads = ThreadModel::Unknown;
  LinkMode link_mode = LinkMode::Default;
  std::vector<std::string> cpu_features;  // lower-case
  std::string sysroot_id;                 // empty when none is needed/provided
  bool sysroot_present = false;
};

// Why a constraint failed. Ordered by the diagnostic categories in PRD §7.7.
enum class RejectionReason {
  None,
  VersionMismatch,
  TargetUnsupported,
  SysrootMissing,
  RuntimeIncompatible,
  DownloadUnavailable,
  OfflineRestriction,
  RegistryVerificationFailed,
  ConstraintUnsatisfied,
  NotProbed,
};

std::string_view to_string(RejectionReason reason);

struct ConstraintVerdict {
  bool satisfied = true;
  std::vector<Constraint> failed;
  std::vector<Constraint> unmet_preferences;
  RejectionReason reason = RejectionReason::None;
  std::string explanation;

  bool ok() const { return satisfied; }
};

// Judges a candidate against a constraint set. Hard constraints must all hold;
// preferences only affect ranking.
ConstraintVerdict evaluate(const CandidateEnvironment& candidate,
                           const std::vector<Constraint>& constraints);

// Checks whether a sysroot manifest's declared target/ABI is usable for a
// given target triple (PRD §8.5, TGT-003).
struct SysrootCompatResult {
  bool compatible = false;
  RejectionReason reason = RejectionReason::None;
  std::string explanation;
};
SysrootCompatResult check_sysroot_compat(const Triple& target,
                                         const std::string& sysroot_target_triple,
                                         Crt sysroot_crt,
                                         LinkMode link_mode,
                                         std::optional<Version> sysroot_min_os);

// A convenience: does this platform rely on glibc symbol versioning?
bool needs_glibc_version_floor(const Triple& triple);

// Human-readable summary of a triple's ABI-relevant traits.
std::string describe_environment(const CandidateEnvironment& env);

}  // namespace cova::platform
