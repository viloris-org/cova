// Cova — ABI model and compatibility rules (PRD §6.3, §8.5, §10.5).
//
// Cova never assumes two toolchains producing "the same version" yield
// interchangeable binaries. This header models the traits that actually decide
// link-time compatibility.
#pragma once

#include <compare>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "platform/triple/triple.hpp"

namespace cova::platform {

// Version of the ABI identity schema. A bump means an older identity cannot be
// silently interpreted as a complete identity under the current rules.
inline constexpr std::string_view kAbiSignatureSchemaVersion = "cova.abi-signature/2";
// Version of the argument classification rules. A rule change invalidates
// effective-ABI identities produced by an older classifier.
inline constexpr std::string_view kAbiClassifierVersion = "cova.abi-classifier/1";

// Whether an ABI fact was actually established. A missing observation is
// Unknown, never a harmless default value.
enum class AbiFactState { Known, Unknown, Unsupported, Conflicting };
std::string_view to_string(AbiFactState state);

// The subject an ABI fact speaks about (FR-15). Toolchain facts do not prove
// source-level pragmas, per-TU options or exported layouts.
enum class AbiEvidenceScope {
  Toolchain,
  CompileAction,
  TranslationUnit,
  ObjectFile,
  ExportedInterface,
  LinkedArtifact,
};
std::string_view to_string(AbiEvidenceScope scope);

// How one compiler argument relates to the binary interface.
enum class AbiArgumentClass {
  AbiAffecting,
  AbiNeutral,
  PotentiallyAbiAffecting,
  Unsupported,
  Conflicting,
};
std::string_view to_string(AbiArgumentClass classification);

struct AbiArgument {
  std::string argument;
  AbiArgumentClass classification = AbiArgumentClass::PotentiallyAbiAffecting;
  // Normalised effect, e.g. "exceptions=0"; empty when neutral or unknown.
  std::string effect;
};

struct AbiFact {
  std::string name;
  std::string value;
  AbiFactState state = AbiFactState::Unknown;
  AbiEvidenceScope scope = AbiEvidenceScope::Toolchain;
  std::string subject;        // toolchain or action identity
  std::string configuration;  // effective flags/config identity
  std::string source;         // "probe:preprocessor-macro", "probe:compiler-driver"
  std::string producer_version;

  auto operator<=>(const AbiFact&) const = default;
  bool operator==(const AbiFact&) const = default;
};

// Stable fingerprint of a fact set: scope, subject, configuration, name, value
// and state all participate, in a deterministic order.
std::string abi_facts_fingerprint(const std::vector<AbiFact>& facts);

// Threading model of a target. Declared here (not in constraints.hpp) because
// AbiSignature needs it, and constraints depends on this header, not vice versa.
enum class ThreadModel { Unknown, Posix, Win32, Single, Wasm };

std::string_view to_string(ThreadModel model);

// A dotted platform version used for deployment-floor constraints
// (macOS 11.0, glibc 2.34, WASI preview1, ...).
struct OsVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;

  static Result<OsVersion> parse(std::string_view text);
  std::string to_string() const;
  auto operator<=>(const OsVersion&) const = default;
  bool operator==(const OsVersion&) const = default;
  bool is_set() const { return major != 0 || minor != 0 || patch != 0; }
};

// The complete set of traits that determine binary compatibility. Two packages
// may share artifacts only when their AbiSignatures are equal (PRD §10.5).
struct AbiSignature {
  Triple triple;
  Abi abi = Abi::Unknown;
  Crt crt = Crt::Unknown;
  LibCpp libcpp = LibCpp::Unknown;
  LinkMode link_mode = LinkMode::Default;
  // Values are meaningful only when the matching state is Known. The defaults
  // are the common case, but the default state is Unknown: Cova never claims
  // an unprobed fact.
  bool exceptions = true;
  AbiFactState exceptions_state = AbiFactState::Unknown;
  bool rtti = true;
  AbiFactState rtti_state = AbiFactState::Unknown;
  ThreadModel threads = ThreadModel::Unknown;
  std::vector<std::string> abi_affecting_features;  // sorted, e.g. "_GLIBCXX_USE_CXX11_ABI=1"
  std::string stdlib_abi_tag;                       // compiler-specific ABI tag
  // Additional probed facts (data layout, standard-library ABI switches).
  std::vector<AbiFact> facts;

  std::string to_string() const;
  // Stable key used inside cache identities.
  std::string fingerprint_key() const;
  auto operator<=>(const AbiSignature&) const = default;
  bool operator==(const AbiSignature&) const = default;
};

enum class AbiVerdict { Compatible, Incompatible, Unknown };

struct AbiCheckResult {
  AbiVerdict verdict = AbiVerdict::Unknown;
  std::vector<std::string> differences;  // human-readable mismatches
  std::string summary;
};

// Compares two signatures for link compatibility.
AbiCheckResult check_abi_compatibility(const AbiSignature& left, const AbiSignature& right);

// Standard-library ABI of a compiler family, e.g. clang defaults to libstdc++ on
// Linux. Used to fill in `AbiSignature::libcpp` when it is not declared.
LibCpp default_libcpp_for(std::string_view compiler_family, const Triple& triple);
Crt default_crt_for(const Triple& triple);

// True when the C++ ABI tag names a known, link-compatible stdlib layout.
bool abi_tags_link_compatible(std::string_view a, std::string_view b);

// Classifies one compiler argument (already split; do not pass argv[0]).
AbiArgument classify_abi_argument(std::string_view argument);

// The effective ABI of one compile action: the toolchain baseline with the
// action's ordered arguments applied. Unknown/potentially-affecting arguments
// contribute an opaque fingerprint and set `state` to Unknown; they are never
// silently treated as ABI neutral.
struct EffectiveAbi {
  AbiSignature signature;
  std::vector<AbiArgument> arguments;
  std::string opaque_fingerprint;
  AbiFactState state = AbiFactState::Known;
  std::vector<std::string> unsupported;

  // Stable identity used by the build action key.
  std::string identity_key() const;
};

EffectiveAbi effective_abi(const AbiSignature& baseline,
                           const std::vector<std::string>& arguments);

// Applies preprocessor facts observed by a toolchain probe. Missing facts stay
// Unknown. `compiler_family` gates facts that are only meaningful for GCC/Clang.
AbiSignature apply_abi_facts(const AbiSignature& baseline, std::string_view compiler_family,
                             const std::map<std::string, std::string, std::less<>>& facts);

// Feature flags that change the ABI and therefore must partition artifact caches.
bool is_abi_affecting_feature(std::string_view feature);

}  // namespace cova::platform
