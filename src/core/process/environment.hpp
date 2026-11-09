// Cova — explicit child-process environments (PRD §9.3, §17.3).
//
// A build action is only reproducible when every environment variable that can
// influence it is known, passed deliberately, and part of the action identity.
// This header models that policy. The default policy is explicit: only audited
// build-relevant variables are forwarded, everything else is rejected, and the
// caller can opt into passing (and thereby tracking, or explicitly not
// tracking) additional variables.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common.hpp"

namespace cova::proc {

// How one environment variable is treated for a child process.
enum class EnvironmentPolicy {
  Explicit,          // set by the caller, not inherited from the parent
  InheritedTracked,  // inherited from the parent and audited into the identity
  Rejected,          // deliberately not visible to the child
  Untracked,         // passed, but its value is not part of the identity
};

std::string_view to_string(EnvironmentPolicy policy);

// How well the environment of an action is understood. Only a hermetic
// environment may be shared or reused across independent executions.
enum class EnvironmentIsolation {
  Hermetic,            // every passed variable is explicit and tracked
  TrackedEnvironment,  // all passed variables are tracked, but the set is extended
  UntrackedEnvironment,  // at least one passed variable is not tracked
};

std::string_view to_string(EnvironmentIsolation isolation);

// One resolved variable in the child environment.
struct EnvironmentEntry {
  std::string name;
  std::string value;
  EnvironmentPolicy policy = EnvironmentPolicy::Explicit;
  bool tracked = true;
};

// A caller-supplied variable. `tracked == false` marks the whole action as an
// untracked environment, which forbids cross-execution reuse.
struct EnvironmentVariable {
  std::string name;
  std::string value;
  bool tracked = true;
};

struct EnvironmentRequest {
  // Copy the whole parent environment, auditing the known build variables and
  // marking everything else untracked. This is the `--inherit-env` escape hatch.
  bool inherit_untracked = false;
  // Explicit additions; they replace an inherited value of the same name.
  std::vector<EnvironmentVariable> additions;
  // Names that must never reach the child.
  std::vector<std::string> removals;
};

// The final, deterministic child environment plus its cache qualification.
struct EnvironmentSnapshot {
  // Sorted by name (case-insensitive on Windows). Contains only variables that
  // are actually passed to the child; rejected names are not stored here.
  std::vector<EnvironmentEntry> entries;
  EnvironmentIsolation isolation = EnvironmentIsolation::Hermetic;
  // Variables deliberately kept away from the child, for diagnostics.
  std::vector<std::string> rejected;
  // Shared/remote artifact reuse requires a hermetic environment.
  bool shared_cache_eligible = true;
  // Local incremental reuse additionally requires a tracked environment.
  bool incremental_eligible = true;

  std::vector<std::pair<std::string, std::string>> variables() const;
  std::map<std::string, std::string, std::less<>> as_map() const;
  // Stable, length-prefixed identity over the policy and every tracked entry.
  // Untracked values are represented only by their names.
  std::string identity() const;
};

// Reads the current process environment as UTF-8 name/value pairs, sorted.
std::vector<std::pair<std::string, std::string>> process_environment();

// The audited policy for one variable name.
EnvironmentPolicy environment_policy(std::string_view name);
// True for variables that can change compilation or linking and are therefore
// inherited and tracked by the default policy.
bool is_tracked_environment_variable(std::string_view name);
// True for variables Cova refuses to forward because forwarding them would
// create an untracked build input (tool overrides, compiler flag bags, ...).
bool is_rejected_environment_variable(std::string_view name);
// True for secret-bearing names; such variables are never forwarded and never
// written to cache metadata or ordinary diagnostics.
bool is_sensitive_environment_variable(std::string_view name);

// Computes the explicit child environment for a request. Returns an error when
// a request tries to pass a rejected or sensitive variable.
Result<EnvironmentSnapshot> collect_environment(const EnvironmentRequest& request = {});

}  // namespace cova::proc
