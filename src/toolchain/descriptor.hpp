// Cova — toolchain descriptors and capability model (PRD §7.5, §7.6).
//
// A toolchain is not "a path to a compiler". It is a descriptor of a verified
// component *set*: driver, linker, archiver, runtime, sysroot constraint, plus
// probed capabilities and a content-derived identity.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "core/hashing/hash.hpp"
#include "core/version/version.hpp"
#include "platform/abi/abi.hpp"
#include "platform/constraints/constraints.hpp"
#include "platform/triple/triple.hpp"

namespace cova::toolchain {

namespace stdfs = std::filesystem;

enum class ToolchainSource {
  System,     // discovered on the host
  Managed,    // installed under ~/.cova/toolchains
  UserPath,   // explicitly configured by the user
  Registry,   // downloaded from a trusted registry
  Unknown,
};

std::string_view to_string(ToolchainSource source);

enum class Policy { SystemFirst, ManagedFirst, SystemOnly, ManagedOnly };
std::string_view to_string(Policy policy);
std::optional<Policy> policy_from_string(std::string_view text);

// Independent capability flags, each established by an actual probe (PRD §7.6).
// A version number alone never implies a capability.
struct ToolchainCapabilities {
  bool c_compilation = false;
  bool cpp_compilation = false;
  bool preprocess = false;
  bool static_library = false;
  bool shared_library = false;
  bool executable_link = false;
  bool cpp26_language = false;     // all required C++26 language features accepted
  bool cpp26_library = false;      // required C++26 library facilities usable
  bool stdlib_headers = false;     // <vector>, <string>, <iostream> found
  bool lto = false;
  bool modules = false;
  bool sanitizers = false;
  bool coverage = false;
  bool debug_info = false;
  bool threads_available = false;
  bool cpp_modules_scan = false;
  // Feature probes keyed by name, for compiler-specific extras.
  std::vector<std::string> probed_features;

  std::string to_string() const;
  bool basic_codegen() const { return cpp_compilation && static_library && executable_link; }
};

// One executable component of a toolchain.
struct ToolComponent {
  std::string role;  // "cc", "cxx", "linker", "archiver", "objcopy", "ranlib", "lld"
  stdfs::path path;
  std::string version_text;
  std::optional<Version> version;
  std::string sha256;  // empty when not yet hashed
};

struct ToolchainDescriptor {
  std::string id;                      // stable identity, e.g. "system-clang-23.1.4"
  std::string family;                  // "clang", "gcc", "msvc"
  Version version;
  ToolchainSource source = ToolchainSource::Unknown;
  // Host platform this toolchain executes on (may differ from its targets).
  platform::Triple host;
  // Targets this toolchain claims to support, as canonical strings.
  std::vector<std::string> supported_targets;
  // Component executables.
  std::vector<ToolComponent> components;
  // Compiler resource/builtin include directory.
  stdfs::path resource_dir;
  // Default sysroot advertised by the compiler, when any.
  stdfs::path default_sysroot;
  // True when this driver already carries the runtime for the targets it names
  // (a prefixed cross GCC, or a cross Clang configured with a sysroot). Such a
  // toolchain does not require a separately installed Cova sysroot.
  bool self_contained = false;
  // Syroot constraints this toolchain requires.
  std::optional<std::string> required_sysroot_target;
  platform::Crt crt = platform::Crt::Unknown;
  platform::LibCpp libcpp = platform::LibCpp::Unknown;
  platform::Abi abi = platform::Abi::Unknown;
  ToolchainCapabilities capabilities;
  // Fingerprint over the component hashes plus probed capabilities (PRD §7.5).
  std::string content_hash;
  // Trust/source metadata for the transparency requirement.
  std::string registry_id;
  std::string trust_level;  // "system", "verified", "unverified"

  bool has_component(std::string_view role) const;
  const ToolComponent* component(std::string_view role) const;
  stdfs::path component_path(std::string_view role) const;
  // "Clang 23.1.4 (system)"
  std::string label() const;
  // Stable signature used in cache keys: id + target + abi-relevant traits.
  platform::CandidateEnvironment to_candidate(const platform::Triple& target) const;
  // Recomputed fingerprint from component hashes; validates `content_hash`.
  std::string compute_fingerprint() const;
  std::string to_string() const;
};

// The frozen result of resolution: everything the build layer needs and nothing
// more. PRD §17.3 requires this to be immutable once build execution starts.
struct ResolvedToolchain {
  ToolchainDescriptor descriptor;
  platform::Triple target;
  platform::AbiSignature abi_signature;
  // Resolved sysroot, if one is required for this target.
  std::string sysroot_id;
  stdfs::path sysroot_path;
  // Content-derived identity of the sysroot (manifest hash or a conservative
  // filesystem fingerprint), so a changed sysroot invalidates build actions.
  std::string sysroot_fingerprint;
  // The frozen environment handed to the build graph.
  platform::CandidateEnvironment environment;
  // Human-readable chain of resolution decisions (transparency, PRD §1.3).
  std::vector<std::string> decisions;
  // Identity string for cache keys, over toolchain + target + sysroot.
  std::string fingerprint;

  std::string to_string() const;
  std::string fingerprint_input() const;
};

}  // namespace cova::toolchain
