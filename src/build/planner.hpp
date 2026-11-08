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
};

struct PlanResult {
  BuildGraph graph;
  std::vector<Path> expected_outputs;
  std::optional<Path> runnable_binary;
  std::vector<Path> test_binaries;
  std::vector<std::string> notes;
};

struct WorkspaceProject {
  manifest::Manifest root_manifest;
  std::vector<manifest::Manifest> members;
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

}  // namespace cova::build
