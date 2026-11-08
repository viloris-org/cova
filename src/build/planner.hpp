// Cova — build planning: manifest + toolchain → Build Graph (PRD §9.1).
//
// The planner is the only layer that turns Cova's model into compiler command
// lines. It never executes anything.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "build/graph/graph.hpp"
#include "core/common.hpp"
#include "manifest/manifest.hpp"
#include "toolchain/descriptor.hpp"

namespace cova::build {

struct PlanOptions {
  platform::Triple target;
  std::string profile = "dev";
  int jobs = 0;
  bool use_color = true;
  bool include_tests = false;
  bool explain = false;
  bool workspace = false;
  // Keeps object files of different packages apart when one target tree holds
  // a root package and its path dependencies. Empty for a standalone package.
  std::string object_namespace;
  // Public interface of the package's transitive dependencies, already rendered
  // as `-I` / `-D` flags. Private settings are never passed here.
  std::vector<std::string> dependency_include_flags;
  std::vector<std::string> dependency_definition_flags;
  // Artifacts of transitive dependencies, dependency-first (link order).
  std::vector<std::string> dependency_artifacts;
};

struct PlanResult {
  BuildGraph graph;
  std::vector<Path> expected_outputs;
  std::optional<Path> runnable_binary;
  std::vector<Path> test_binaries;
  std::vector<std::string> notes;
  // The action that produces the package's primary artifact (archive or link).
  std::optional<ActionId> final_action;
};

struct WorkspaceProject {
  manifest::Manifest root_manifest;
  std::vector<manifest::Manifest> members;
  // Packages the command acts on. Empty means every member is a root. Members
  // reached only as dependencies of a root are built, but their tests are not.
  std::vector<std::string> roots;
};

// Plans one package. `target_dir` is the project's `target/` directory.
Result<PlanResult> plan_package(const manifest::Manifest& manifest,
                                const toolchain::ResolvedToolchain& toolchain,
                                const PlanOptions& options, const Path& target_dir);

// Plans every member, merging their graphs and wiring intra-workspace
// dependency edges.
Result<PlanResult> plan_workspace(const WorkspaceProject& project,
                                  const toolchain::ResolvedToolchain& toolchain,
                                  const PlanOptions& options, const Path& target_dir);

// The directory-safe target component used by the CLI and planner.
std::string target_component(const platform::Triple& target);

// Renders a standard compilation database (`compile_commands.json`) for every
// compile action in the plan, using the exact arguments the planner will run.
std::string compile_commands_json(const PlanResult& plan);

}  // namespace cova::build
