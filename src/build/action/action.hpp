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
#include "platform/abi/abi.hpp"
#include "platform/triple/triple.hpp"

namespace cova::build {

namespace stdfs = std::filesystem;
using Path = stdfs::path;

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

// One input file with its role ("source", "header", "object", "library").
struct FileInput {
  Path path;
  std::string role;
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
  std::map<std::string, std::string, std::less<>> environment;
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
  // Set by the executor before the incremental check; not part of planning.
  mutable std::string computed_key;

  // Deterministic cache identity over action type, argv, input contents,
  // toolchain, sysroot, target, ABI and relevant environment (PRD §9.3).
  std::string cache_key(const ActionEnvironment& env) const;
  std::string label_with_output() const;
};

}  // namespace cova::build
