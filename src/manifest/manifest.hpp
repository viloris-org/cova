// Cova — manifest model (PRD §5.3).
//
// `cova.toml` is the only declaration a project needs. Parsing is strict about
// structure and generous about formatting: unknown keys warn, wrong types fail,
// and every error carries the offending line.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "core/toml/toml.hpp"
#include "core/version/version.hpp"
#include "platform/constraints/constraints.hpp"
#include "platform/triple/triple.hpp"
#include "toolchain/descriptor.hpp"

namespace cova::manifest {

namespace stdfs = std::filesystem;

enum class PackageKind { Executable, StaticLibrary, SharedLibrary };

std::string_view to_string(PackageKind kind);
std::optional<PackageKind> kind_from_string(std::string_view text);

// [package]
struct PackageSection {
  std::string name;
  std::string version = "0.1.0";
  std::string edition = "c++26";  // C++ standard selector
  std::string description;
  std::vector<std::string> authors;
  std::string license;
  // Declared standard library / runtime preferences.
  std::optional<platform::Crt> crt;
  std::optional<platform::LibCpp> libcpp;

  // C++ standard as an integer: 26 for "c++26", 23 for "c++23", and the same
  // for the GNU spellings (`gnu++23`). Returns 0 when the edition is not a
  // recognised C++ standard, so a caller can reject it instead of silently
  // compiling as the newest standard.
  int cpp_standard() const;
};

// [build]
struct BuildSection {
  PackageKind kind = PackageKind::Executable;
  std::vector<std::string> sources;              // globs relative to package root
  std::vector<std::string> source_dirs;          // convenience: dirs to scan
  std::vector<std::string> public_includes;      // exposed to dependents
  std::vector<std::string> private_includes;
  std::vector<std::string> public_definitions;
  std::vector<std::string> private_definitions;
  std::vector<std::string> compile_options;
  std::vector<std::string> link_options;
  std::vector<std::string> link_libraries;       // system-ish libraries by name
  std::vector<std::string> exclude;              // globs excluded from sources
  std::string entry_point;                       // optional explicit main source
  bool build_script = false;                     // project ships build.cova.toml
  // Declared default target ("aarch64-unknown-linux-gnu"); empty = host default.
  // Added additively by the manifest owner so `Manifest::declared_target()` has
  // an explicit declaration to report.
  std::string target;
};

// [dependencies] / [dev-dependencies]
struct DependencySpec {
  std::string name;
  // Version requirement (empty for path/git dependencies).
  std::vector<VersionRequirement> requirements;
  std::string requirement_text;
  // Sources (PRD §10.3).
  std::optional<stdfs::path> path;
  std::optional<std::string> git;
  std::optional<std::string> git_rev;   // tag/branch/rev
  std::optional<std::string> registry;  // registry name
  // Feature selection (PRD §10.7).
  std::vector<std::string> features;
  bool optional = false;
  bool dev = false;
  // Platform-conditioned dependency: only active for these targets.
  std::vector<std::string> target_filters;

  bool is_local() const { return path.has_value(); }
  bool is_git() const { return git.has_value(); }
  bool is_registry() const { return registry.has_value() || (!path && !git); }
  std::string to_string() const;
};

// [profile.dev] / [profile.release]
struct Profile {
  std::string name = "dev";
  int optimization = 0;        // -O level, always 0..3
  // True when the profile asked for size optimisation (`optimization = "s"` /
  // `"size"`). It is a separate flag because `-Os` is not an optimisation
  // *level*: mapping "size" onto `-O2` silently produced a speed-optimised
  // binary when the manifest asked for the smallest one.
  bool optimize_for_size = false;
  bool debug = true;           // debug info
  bool lto = false;
  bool sanitize = false;
  std::string sanitizer;       // "address", "undefined", "thread"
  bool assertions = true;
  std::vector<std::string> extra_cxxflags;
  std::vector<std::string> extra_ldflags;
  std::string to_string() const;
  // Compiler flags contributed by this profile (compiler-family independent).
  std::vector<std::string> cxx_flags() const;
  std::vector<std::string> ld_flags() const;
};

// [toolchain]
struct ToolchainSection {
  toolchain::Policy policy = toolchain::Policy::SystemFirst;
  std::string compiler;                 // family, empty = any
  std::string version;                  // requirement text, empty = any
  bool auto_install = true;
  std::string preferred;                // pinned toolchain id
};

// [format]
struct FormatSection {
  std::string tool = "clang-format";
  std::string style = "file";           // or an inline clang-format style
  std::vector<std::string> include;     // extra globs
  std::vector<std::string> exclude;
  std::string fallback_style = "LLVM";
};

// [targets.<triple>]  — target-specific overrides (PRD §6.4 TGT-005)
struct TargetOverride {
  std::string target;
  std::optional<PackageKind> kind;
  std::vector<std::string> definitions;
  std::vector<std::string> compile_options;
  std::vector<std::string> link_options;
  std::vector<std::string> dependencies;  // dependency names enabled for this target
};

// [test]
struct TestSection {
  // Glob patterns for test sources. The default is enabled so a manifest with
  // no `[test]` section still finds `tests/**`; writing `sources = []` disables
  // discovery explicitly (the parser distinguishes the two cases).
  std::vector<std::string> sources = {"tests/**/*.cpp"};
  std::vector<std::string> exclude;
  std::string runner;                    // "", "native", "qemu", "wine", "remote"
  std::string runner_command;
};

// [workspace] — only meaningful at a workspace root
struct WorkspaceSection {
  std::vector<std::string> members;
  std::vector<std::string> exclude;
  // Shared settings inherited by members unless overridden.
  std::optional<ToolchainSection> toolchain;
  std::optional<FormatSection> format;
};

struct Manifest {
  // Provenance.
  stdfs::path path;       // path to cova.toml
  stdfs::path root;       // package root (dir containing cova.toml)
  std::string source_name;

  PackageSection package;
  BuildSection build;
  std::vector<DependencySpec> dependencies;
  std::map<std::string, Profile, std::less<>> profiles;
  ToolchainSection toolchain;
  FormatSection format;
  TestSection test;
  std::optional<WorkspaceSection> workspace;
  std::map<std::string, TargetOverride, std::less<>> target_overrides;
  // Extra constraints from [build] keys (PRD §6.3).
  std::vector<platform::Constraint> constraints;

  // Convenience accessors.
  const Profile& profile(std::string_view name) const;
  bool is_workspace_root() const;
  bool has_package() const { return !package.name.empty(); }
  std::vector<const DependencySpec*> target_dependencies(const platform::Triple& target) const;
  // Manifest-declared target if any (empty = host default).
  std::optional<platform::Triple> declared_target() const;
};

struct ManifestLoadOptions {
  // Emit warnings for unknown keys instead of ignoring them silently.
  bool strict = false;
  // Fill in defaults for omitted sections.
  bool apply_defaults = true;
};

struct ManifestLoadResult {
  Manifest manifest;
  std::vector<std::string> warnings;  // rendered by the CLI
};

// Loads `<root>/cova.toml`.
Result<ManifestLoadResult> load(const stdfs::path& manifest_path,
                                const ManifestLoadOptions& options = {});
Result<ManifestLoadResult> load_from_root(const stdfs::path& root,
                                          const ManifestLoadOptions& options = {});

// Parses already-read TOML text (tests, `cova init` previews).
Result<ManifestLoadResult> parse(std::string_view text, std::string_view source_name,
                                 const stdfs::path& root,
                                 const ManifestLoadOptions& options = {});

// Serialises a manifest back to TOML. Round-trips through `parse`.
std::string to_toml(const Manifest& manifest);

// Creates the default manifest used by `cova new` (PRD §5.1).
Manifest default_manifest(std::string_view name, PackageKind kind);
// Default manifest for a workspace root.
Manifest default_workspace_manifest(std::string_view name);

// Default profile set (dev/release) per PRD §5.3.
std::map<std::string, Profile, std::less<>> default_profiles();

// Validates semantic rules: name syntax, kind/source consistency, duplicate deps.
Result<std::vector<std::string>> validate(const Manifest& manifest);

// Finds the nearest cova.toml at or above `start`.
Result<stdfs::path> find_manifest_upwards(const stdfs::path& start);

}  // namespace cova::manifest
