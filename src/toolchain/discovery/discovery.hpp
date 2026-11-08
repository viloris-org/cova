// Cova — system toolchain discovery (PRD §7.2 step 1).
//
// Discovery is deliberately side-effect free: it locates candidate compilers
// and reads their version banners. Capabilities are established separately by
// the probe layer, because a version banner proves very little.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "toolchain/descriptor.hpp"

namespace cova::toolchain {

// Where discovery looks, in order.
struct DiscoveryOptions {
  bool include_system = true;  // /usr/bin, /usr/local/bin, PATH
  bool include_managed = true; // ~/.cova/toolchains
  bool include_user_path = true;
  // Extra directories to scan, highest priority.
  std::vector<std::filesystem::path> extra_roots;
  // Restrict families, empty = all known families.
  std::vector<std::string> families; // "clang", "gcc"
  // Root to scan for managed toolchains; defaults to ~/.cova/toolchains.
  std::filesystem::path managed_root;
  // Timeout for each version probe invocation.
  int probe_timeout_ms = 3000;
};

// Candidates discovered on this host, de-duplicated by canonical executable
// path. Ordered: explicit roots, PATH, managed store, standard system dirs.
Result<std::vector<ToolchainDescriptor>> discover(const DiscoveryOptions& options);

// Discovers a single toolchain rooted at a specific compiler binary. Used by
// tests and by `--toolchain <path>`.
Result<ToolchainDescriptor> describe_compiler(const std::filesystem::path& compiler_path,
                                              ToolchainSource source);

// Parses `clang version 23.1.4 (...)` / `g++ (GCC) 16.2.1 20260810`.
// Returns nullopt when the banner is not recognised.
std::optional<Version> parse_version_banner(std::string_view banner);

// Maps an executable name to a compiler family ("clang", "gcc", "clang-cl").
// Handles version suffixes (`g++-14`) and cross prefixes
// (`aarch64-linux-gnu-g++`).
std::optional<std::string> family_from_executable(std::string_view filename);

// Derives the target a prefixed cross driver is configured for, e.g.
// `aarch64-linux-gnu-g++` -> aarch64-unknown-linux-gnu. A host-native driver
// (`g++`, `clang++-18`) returns nullopt.
std::optional<platform::Triple> target_from_executable(std::string_view filename);

// Locates the sibling linker/archiver for a compiler driver.
struct SiblingTools {
  std::filesystem::path linker;   // ld.lld, ld, lld-link
  std::filesystem::path archiver; // llvm-ar, ar
  std::filesystem::path objcopy;  // llvm-objcopy, objcopy
};
SiblingTools find_sibling_tools(const std::filesystem::path& compiler_path,
                                std::string_view family);

// Reads the compiler's resource directory (`clang -print-resource-dir`).
Result<std::filesystem::path> query_resource_dir(const std::filesystem::path& compiler_path);

// What the compiler reports as its default sysroot (`--print-sysroot`), if any.
Result<std::filesystem::path> query_default_sysroot(const std::filesystem::path& compiler_path);

// Lists targets the compiler claims (`--print-targets`). Empty when
// unavailable.
std::vector<std::string> query_supported_targets(const std::filesystem::path& compiler_path);

// Scans a managed toolchain directory for installed descriptors.
Result<std::vector<ToolchainDescriptor>>
discover_managed(const std::filesystem::path& toolchains_root);

} // namespace cova::toolchain
