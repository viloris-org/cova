// Cova — ABI model and compatibility rules (PRD §6.3, §8.5, §10.5).
//
// Cova never assumes two toolchains producing "the same version" yield
// interchangeable binaries. This header models the traits that actually decide
// link-time compatibility.
#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "platform/triple/triple.hpp"

namespace cova::platform {

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
  bool exceptions = true;
  bool rtti = true;
  ThreadModel threads = ThreadModel::Unknown;
  std::vector<std::string> abi_affecting_features;  // sorted, e.g. "_GLIBCXX_USE_CXX11_ABI=1"
  std::string stdlib_abi_tag;                       // compiler-specific ABI tag

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

// Feature flags that change the ABI and therefore must partition artifact caches.
bool is_abi_affecting_feature(std::string_view feature);

}  // namespace cova::platform
