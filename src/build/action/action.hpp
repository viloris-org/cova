// Cova — Build Graph action model (PRD §9.1, §17.2).
//
// An Action is a pure description: it names its inputs, outputs, arguments and
// dependencies, and never embeds compiler policy. Providers (the planner) emit
// actions; the executor runs them; the cache restores them.
#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "core/process/environment.hpp"
#include "platform/abi/abi.hpp"
#include "platform/triple/triple.hpp"

namespace cova::build {

namespace stdfs = std::filesystem;
using Path = stdfs::path;

// Version of the action identity schema. A bump deliberately invalidates every
// cache entry produced by an older meaning of the identity.
inline constexpr std::string_view kActionIdentitySchemaVersion = "cova.action-identity/2";

enum class ActionKind {
  Compile,
  Archive,
  Link,
  Run,
  Copy,
  Custom,
};

std::string_view to_string(ActionKind kind);

// Where an action executes. The artifact target is separate from both.
enum class ExecPlatform {
  BuildHost,
  TargetDevice,
  RemoteHost,
  Unknown,
};

std::string_view to_string(ExecPlatform platform);

// Where an input was learned from. Discovered inputs are the provider's input
// closure; observed inputs were verified during/after execution.
enum class InputOrigin {
  Declared,
  Discovered,
  Observed,
};

std::string_view to_string(InputOrigin origin);

// One input file with its role ("source", "header", "object", "library").
struct FileInput {
  Path path;
  std::string role;
  InputOrigin origin = InputOrigin::Declared;
};

// One output file. `primary` marks the artifact the action exists to produce.
struct FileOutput {
  Path path;
  bool primary = false;
};

struct ActionId {
  std::uint64_t value = 0;
  auto operator<=>(const ActionId&) const = default;
  bool operator==(const ActionId&) const = default;
};

// Frozen environment for cache identity (PRD §9.3, §17.3).
struct ActionEnvironment {
  platform::Triple target;
  platform::AbiSignature abi;
  std::string toolchain_fingerprint;
  std::string sysroot_fingerprint;
  // The exact child environment. When `explicit_environment` is true the
  // executor passes only these variables; otherwise the child inherits the
  // parent environment (legacy behaviour, not cache-safe).
  std::map<std::string, std::string, std::less<>> environment;
  bool explicit_environment = false;
  proc::EnvironmentIsolation isolation = proc::EnvironmentIsolation::TrackedEnvironment;
  // Shared/remote cache qualification; computed from the environment policy and
  // the action's input closure.
  bool shared_cache_eligible = false;
  // Local incremental reuse qualification.
  bool incremental_eligible = true;
};

struct Action {
  ActionId id;
  ActionKind kind = ActionKind::Compile;
  ExecPlatform exec_platform = ExecPlatform::BuildHost;
  std::string label;
  std::string package;
  std::vector<FileInput> inputs;
  std::vector<FileOutput> outputs;
  std::vector<ActionId> dependencies;
  // argv[0] is the program; the rest are arguments. No shell is involved.
  std::vector<std::string> argv;
  Path working_dir;
  bool cacheable = true;
  // False until the provider's discovered input closure is complete (for a
  // compile action: its depfile parsed successfully). An incomplete closure
  // must not be reused across executions.
  bool input_closure_complete = false;
  // Set by the executor before the incremental check; not part of planning.
  mutable std::string computed_key;
  // False when an identity input could not be enumerated/read completely
  // (for example an unreadable include directory). Such an action must not be
  // reused from the cache or from incremental state.
  mutable bool input_identity_complete = true;
  // Content hashes captured by the last cache_key computation, used to detect
  // inputs changed by another process between discovery and execution.
  mutable std::vector<std::pair<std::string, std::string>> computed_input_hashes;

  // Deterministic cache identity over the identity schema, action type, argv
  // (including response-file contents), input contents, include directories,
  // toolchain, sysroot, target, effective ABI and relevant environment.
  std::string cache_key(const ActionEnvironment& env) const;
  // Re-hashes the inputs captured by the last cache_key call. Returns false and
  // sets `detail` when an input changed or disappeared.
  bool inputs_stable(std::string* detail = nullptr) const;
  std::string label_with_output() const;
};

}  // namespace cova::build
