// Cova — semantic versions and version requirement matching (PRD §10.7, §7.2).
//
// Requirements accept the subset Cova documents for [toolchain] and
// [dependencies]:
//
//   "11"                -> >=11, <12        (bare major means that major series)
//   "1.15"              -> >=1.15, <1.16
//   "=1.15.2" / "1.15.2"-> exactly 1.15.2
//   "^1.2.3"            -> >=1.2.3, <2.0.0
//   "~1.2.3"            -> >=1.2.3, <1.3.0
//   ">=20, <23"         -> conjunction
//   "*"                 -> any
#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova {

class Version {
 public:
  Version() = default;
  Version(int major, int minor, int patch, std::string pre = {});
  explicit Version(std::string_view text) = delete;

  static Result<Version> parse(std::string_view text);
  // Best-effort numeric extraction from tool output such as
  // "clang version 23.1.4" or "g++ (GCC) 16.2.1 20260810".
  static Result<Version> extract(std::string_view text);

  int major = 0;
  int minor = 0;
  int patch = 0;
  std::string pre;  // pre-release tag, empty for releases

  std::string to_string() const;
  std::string to_string_short() const;  // "23.1" style, drops trailing zeros

  auto operator<=>(const Version&) const = default;
  bool operator==(const Version&) const = default;
};

struct VersionBound {
  enum class Op { Eq, Gt, Ge, Lt, Le };
  Op op = Op::Ge;
  Version version;
  bool matches(const Version& candidate) const;
  std::string to_string() const;
};

class VersionRequirement {
 public:
  VersionRequirement() = default;  // "*" — matches anything

  static Result<VersionRequirement> parse(std::string_view text);
  static VersionRequirement any();

  bool matches(const Version& candidate) const;
  bool is_any() const;
  // True when the requirement pins exactly one version (used by locked mode).
  bool is_exact() const;
  std::optional<Version> exact_version() const;

  // Bounds are a conjunction (all must hold) and kept sorted for stable output.
  const std::vector<VersionBound>& bounds() const { return bounds_; }
  std::string to_string() const;

  bool operator==(const VersionRequirement&) const = default;

 private:
  std::vector<VersionBound> bounds_;
  bool exact_ = false;
  bool any_ = true;
};

// Compares dotted versions such as "0.1.0" and tolerates a missing patch.
bool version_satisfies(std::string_view version_text, std::string_view requirement_text);

}  // namespace cova
