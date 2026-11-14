// Cova — semantic versions and version requirement matching (implementation).
//
// The requirement grammar Cova documents (see version.hpp) is intentionally
// smaller than Cargo's: no wildcard sections beyond "*", no `||`, no unions.
// Anything unparseable is rejected with "cova::version::requirement" rather
// than silently treated as a match — a wrong match would select the wrong
// toolchain, which PRD §7.2 calls out as a correctness bug.
#include "core/version/version.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova {
namespace {

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Parses a non-negative decimal integer from `text`; on success advances `pos`.
bool parse_number(std::string_view text, std::size_t& pos, int& out) {
  const std::size_t begin = pos;
  long long value = 0;
  while (pos < text.size() && is_digit(text[pos])) {
    value = value * 10 + (text[pos] - '0');
    if (value > 1'000'000'000) {
      return false;  // implausible component; reject rather than overflow
    }
    ++pos;
  }
  if (pos == begin) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

// Compares two numeric SemVer identifiers ("1", "007", "99999999999999999999").
//
// SemVer places no length limit on a numeric pre-release identifier, and Cova
// must not abort on a version a user wrote in a manifest, so this never
// converts to an integer: leading zeros are dropped, then the longer
// significant part is the larger number, and equal lengths compare
// lexicographically. Both inputs are non-empty and all-digit by construction.
int compare_numeric_identifier(std::string_view lhs, std::string_view rhs) {
  const std::size_t lhs_begin = lhs.find_first_not_of('0');
  const std::size_t rhs_begin = rhs.find_first_not_of('0');
  const std::string_view l = lhs_begin == std::string_view::npos ? std::string_view{}
                                                                 : lhs.substr(lhs_begin);
  const std::string_view r = rhs_begin == std::string_view::npos ? std::string_view{}
                                                                 : rhs.substr(rhs_begin);
  if (l.size() != r.size()) {
    return l.size() < r.size() ? -1 : 1;
  }
  if (l != r) {
    return l < r ? -1 : 1;
  }
  return 0;
}

// SemVer pre-release comparison: a release outranks any pre-release; otherwise
// dot-separated identifiers compare numerically when both are numeric and
// lexically when either is alphanumeric.
int compare_prerelease(std::string_view a, std::string_view b) {
  if (a.empty() && b.empty()) {
    return 0;
  }
  if (a.empty()) {
    return 1;  // release > pre-release
  }
  if (b.empty()) {
    return -1;
  }
  std::vector<std::string> lhs = str::split(a, '.');
  std::vector<std::string> rhs = str::split(b, '.');
  const std::size_t n = std::min(lhs.size(), rhs.size());
  for (std::size_t i = 0; i < n; ++i) {
    const bool lhs_num = !lhs[i].empty() &&
                         std::all_of(lhs[i].begin(), lhs[i].end(), [](char c) { return is_digit(c); });
    const bool rhs_num = !rhs[i].empty() &&
                         std::all_of(rhs[i].begin(), rhs[i].end(), [](char c) { return is_digit(c); });
    if (lhs_num && rhs_num) {
      const int order = compare_numeric_identifier(lhs[i], rhs[i]);
      if (order != 0) {
        return order;
      }
      continue;
    }
    if (lhs_num != rhs_num) {
      return lhs_num ? -1 : 1;  // numeric identifiers sort lower
    }
    if (lhs[i] != rhs[i]) {
      return lhs[i] < rhs[i] ? -1 : 1;
    }
  }
  if (lhs.size() != rhs.size()) {
    return lhs.size() < rhs.size() ? -1 : 1;
  }
  return 0;
}

int compare_versions(const Version& a, const Version& b) {
  if (a.major != b.major) {
    return a.major < b.major ? -1 : 1;
  }
  if (a.minor != b.minor) {
    return a.minor < b.minor ? -1 : 1;
  }
  if (a.patch != b.patch) {
    return a.patch < b.patch ? -1 : 1;
  }
  return compare_prerelease(a.pre, b.pre);
}

}  // namespace

// The public relational operators delegate to the SemVer-aware comparison.
// Keeping one implementation means the resolver's "newest compiler" sort and
// requirement matching can never disagree about ordering.
std::strong_ordering Version::operator<=>(const Version& other) const {
  const int order = compare_versions(*this, other);
  if (order < 0) {
    return std::strong_ordering::less;
  }
  if (order > 0) {
    return std::strong_ordering::greater;
  }
  return std::strong_ordering::equal;
}

bool Version::operator==(const Version& other) const { return compare_versions(*this, other) == 0; }

namespace {std::string format_bound(const VersionBound& bound) {
  std::string_view op;
  switch (bound.op) {
    case VersionBound::Op::Eq:
      op = "=";
      break;
    case VersionBound::Op::Gt:
      op = ">";
      break;
    case VersionBound::Op::Ge:
      op = ">=";
      break;
    case VersionBound::Op::Lt:
      op = "<";
      break;
    case VersionBound::Op::Le:
      op = "<=";
      break;
  }
  return std::string(op) + bound.version.to_string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

Version::Version(int major, int minor, int patch, std::string pre)
    : major(major), minor(minor), patch(patch), pre(std::move(pre)) {}

Result<Version> Version::parse(std::string_view text) {
  const std::string_view trimmed = str::trim_view(text);
  if (trimmed.empty()) {
    return std::unexpected(Error::make("cova::version::parse", "empty version string"));
  }

  Version version;
  std::size_t pos = 0;
  if (!parse_number(trimmed, pos, version.major)) {
    return std::unexpected(Error::make("cova::version::parse", "version does not start with a number")
                    .with("input: " + std::string(trimmed)));
  }

  int components = 1;
  if (pos < trimmed.size() && trimmed[pos] == '.') {
    ++pos;
    if (!parse_number(trimmed, pos, version.minor)) {
      return std::unexpected(Error::make("cova::version::parse", "expected a minor version number")
                      .with("input: " + std::string(trimmed)));
    }
    ++components;
    if (pos < trimmed.size() && trimmed[pos] == '.') {
      ++pos;
      if (!parse_number(trimmed, pos, version.patch)) {
        return std::unexpected(Error::make("cova::version::parse", "expected a patch version number")
                        .with("input: " + std::string(trimmed)));
      }
      ++components;
    }
  }
  (void)components;

  if (pos < trimmed.size() && trimmed[pos] == '-') {
    ++pos;
    const std::size_t begin = pos;
    while (pos < trimmed.size() && trimmed[pos] != '+') {
      const char c = trimmed[pos];
      if (!(is_digit(c) || is_alpha(c) || c == '.' || c == '-')) {
        return std::unexpected(Error::make("cova::version::parse", "invalid pre-release tag")
                        .with("input: " + std::string(trimmed)));
      }
      ++pos;
    }
    if (pos == begin) {
      return std::unexpected(Error::make("cova::version::parse", "empty pre-release tag")
                      .with("input: " + std::string(trimmed)));
    }
    version.pre = std::string(trimmed.substr(begin, pos - begin));
  }

  if (pos < trimmed.size() && trimmed[pos] == '+') {
    ++pos;
    const std::size_t begin = pos;
    while (pos < trimmed.size()) {
      const char c = trimmed[pos];
      if (!(is_digit(c) || is_alpha(c) || c == '.') && c != '-') {
        return std::unexpected(Error::make("cova::version::parse", "invalid build metadata")
                        .with("input: " + std::string(trimmed)));
      }
      ++pos;
    }
    if (pos == begin) {
      return std::unexpected(Error::make("cova::version::parse", "empty build metadata")
                      .with("input: " + std::string(trimmed)));
    }
  }

  if (pos != trimmed.size()) {
    return std::unexpected(Error::make("cova::version::parse", "trailing characters after version")
                    .with("input: " + std::string(trimmed))
                    .with("column: " + std::to_string(pos + 1)));
  }
  return version;
}

Result<Version> Version::extract(std::string_view text) {
  // Scan for the first token that looks like a dotted version number. Tool
  // banners are full of noise ("Apple clang version 15.0.0 (clang-1500.3.9.4)"),
  // so tokens are delimited by anything that cannot appear in a version.
  std::size_t i = 0;
  auto ident_char = [](char c) { return is_digit(c) || is_alpha(c) || c == '_'; };
  while (i < text.size()) {
    // `x86_64` contains 86, but it is part of an identifier, not a version.
    const bool boundary = i == 0 || !(ident_char(text[i - 1]) || text[i - 1] == '.');
    if (!boundary || !is_digit(text[i])) {
      ++i;
      continue;
    }
    std::size_t pos = i;
    int major = 0;
    int minor = 0;
    int patch = 0;
    int count = 0;
    if (!parse_number(text, pos, major)) {
      ++i;
      continue;
    }
    ++count;
    if (pos < text.size() && text[pos] == '.') {
      const std::size_t save = pos;
      ++pos;
      if (parse_number(text, pos, minor)) {
        ++count;
        if (pos < text.size() && text[pos] == '.') {
          ++pos;
          if (parse_number(text, pos, patch)) {
            ++count;
          } else {
            pos = save;
          }
        }
      } else {
        pos = save;
      }
    }
    // The token must end here (or with a pre-release tag) to be a version.
    const bool token_end = pos >= text.size() || !(ident_char(text[pos]) || text[pos] == '.');
    if (count >= 1 && token_end) {
      std::string_view candidate = text.substr(i, pos - i);
      auto parsed = Version::parse(candidate);
      if (parsed) {
        return parsed;
      }
    }
    i = pos > i ? pos : i + 1;
  }
  return std::unexpected(Error::make("cova::version::extract", "no version number found in text")
                  .with("text: " + str::collapse_whitespace(text)));
}

std::string Version::to_string() const {
  std::string out = std::to_string(major) + "." + std::to_string(minor) + "." +
                    std::to_string(patch);
  if (!pre.empty()) {
    out += "-";
    out += pre;
  }
  return out;
}

std::string Version::to_string_short() const {
  std::string out = std::to_string(major);
  if (minor != 0 || patch != 0) {
    out += ".";
    out += std::to_string(minor);
  }
  if (patch != 0) {
    out += ".";
    out += std::to_string(patch);
  }
  if (!pre.empty()) {
    out += "-";
    out += pre;
  }
  return out;
}

// ---------------------------------------------------------------------------
// VersionBound
// ---------------------------------------------------------------------------

bool VersionBound::matches(const Version& candidate) const {
  const int order = compare_versions(candidate, version);
  switch (op) {
    case Op::Eq:
      return order == 0;
    case Op::Gt:
      return order > 0;
    case Op::Ge:
      return order >= 0;
    case Op::Lt:
      return order < 0;
    case Op::Le:
      return order <= 0;
  }
  return false;
}

std::string VersionBound::to_string() const { return format_bound(*this); }

// ---------------------------------------------------------------------------
// VersionRequirement
// ---------------------------------------------------------------------------

VersionRequirement VersionRequirement::any() {
  VersionRequirement requirement;
  requirement.any_ = true;
  requirement.exact_ = false;
  return requirement;
}

namespace {

// Splits a requirement on commas, trimming each clause.
std::vector<std::string> split_clauses(std::string_view text) {
  std::vector<std::string> clauses;
  std::size_t begin = 0;
  for (;;) {
    const std::size_t pos = text.find(',', begin);
    std::string clause;
    if (pos == std::string_view::npos) {
      clause = str::trim(text.substr(begin));
      if (!clause.empty()) {
        clauses.push_back(std::move(clause));
      }
      break;
    }
    clause = str::trim(text.substr(begin, pos - begin));
    if (!clause.empty()) {
      clauses.push_back(std::move(clause));
    }
    begin = pos + 1;
  }
  return clauses;
}

struct PartialVersion {
  int parts[3] = {0, 0, 0};
  int count = 0;
  std::string pre;
};

bool parse_partial(std::string_view text, PartialVersion& out) {
  std::size_t pos = 0;
  for (int i = 0; i < 3; ++i) {
    if (i != 0) {
      if (pos >= text.size() || text[pos] != '.') {
        break;
      }
      ++pos;
    }
    if (!parse_number(text, pos, out.parts[i])) {
      return false;
    }
    out.count = i + 1;
  }
  if (pos < text.size() && text[pos] == '-') {
    std::size_t pre_pos = pos + 1;
    const std::size_t begin = pre_pos;
    while (pre_pos < text.size() && text[pre_pos] != '+' &&
           (is_digit(text[pre_pos]) || is_alpha(text[pre_pos]) || text[pre_pos] == '.' ||
            text[pre_pos] == '-')) {
      ++pre_pos;
    }
    if (pre_pos > begin) {
      out.pre = std::string(text.substr(begin, pre_pos - begin));
      pos = pre_pos;
    }
  }
  if (pos < text.size() && text[pos] == '+') {
    ++pos;
    while (pos < text.size()) {
      ++pos;
    }
  }
  return pos == text.size() && out.count > 0;
}

Version partial_to_version(const PartialVersion& p) {
  Version version(p.parts[0], p.parts[1], p.parts[2], p.pre);
  return version;
}

// First version after the series described by a partial requirement:
// `1` -> 2.0.0, `1.2` -> 1.3.0, `1.2.3` -> 1.2.4.
Version next_series(const PartialVersion& p) {
  if (p.count <= 1) {
    return Version(p.parts[0] + 1, 0, 0);
  }
  if (p.count == 2) {
    return Version(p.parts[0], p.parts[1] + 1, 0);
  }
  return Version(p.parts[0], p.parts[1], p.parts[2] + 1);
}

}  // namespace

Result<VersionRequirement> VersionRequirement::parse(std::string_view text) {
  const std::string trimmed = str::trim(text);
  if (trimmed.empty() || trimmed == "*") {
    return VersionRequirement::any();
  }

  VersionRequirement requirement;
  requirement.any_ = false;

  const std::vector<std::string> clauses = split_clauses(trimmed);
  if (clauses.empty()) {
    return std::unexpected(Error::make("cova::version::requirement", "empty version requirement")
                    .with("input: " + std::string(trimmed)));
  }

  for (const std::string& clause : clauses) {
    std::string_view rest = clause;
    std::string_view op_text;
    if (str::starts_with(rest, ">=") || str::starts_with(rest, "<=") ||
        str::starts_with(rest, "==") || str::starts_with(rest, "!=")) {
      op_text = rest.substr(0, 2);
      rest = str::trim_view(rest.substr(2));
    } else if (!rest.empty() &&
               (rest[0] == '>' || rest[0] == '<' || rest[0] == '=' || rest[0] == '^' ||
                rest[0] == '~')) {
      op_text = rest.substr(0, 1);
      rest = str::trim_view(rest.substr(1));
    }

    PartialVersion partial;
    if (!parse_partial(rest, partial)) {
      return std::unexpected(Error::make("cova::version::requirement", "invalid version in requirement")
                      .with("clause: " + clause)
                      .with("input: " + std::string(trimmed)));
    }

    auto add_bound = [&requirement](VersionBound::Op op, const Version& version) {
      requirement.bounds_.push_back(VersionBound{op, version});
    };

    if (op_text.empty()) {
      // Bare requirement: same major/minor series, or an exact pin when all
      // three components are given.
      if (partial.count == 3) {
        add_bound(VersionBound::Op::Eq, partial_to_version(partial));
        requirement.exact_ = true;
      } else if (partial.count == 2) {
        add_bound(VersionBound::Op::Ge,
                  Version(partial.parts[0], partial.parts[1], 0, partial.pre));
        add_bound(VersionBound::Op::Lt, Version(partial.parts[0], partial.parts[1] + 1, 0));
      } else {
        add_bound(VersionBound::Op::Ge, Version(partial.parts[0], 0, 0, partial.pre));
        add_bound(VersionBound::Op::Lt, Version(partial.parts[0] + 1, 0, 0));
      }
    } else if (op_text == "=" || op_text == "==") {
      add_bound(VersionBound::Op::Eq, partial_to_version(partial));
      requirement.exact_ = true;
    } else if (op_text == "!=") {
      return std::unexpected(Error::make("cova::version::requirement",
                              "the != operator is not supported in version requirements")
                      .with("clause: " + clause));
    } else if (op_text == ">") {
      // `>1.2` means "newer than the whole 1.2 series", i.e. >= 1.3.0. Taking
      // `>1.2.0` would have accepted 1.2.1, contradicting the series reading
      // used by the bare form (`1.2` == `>=1.2, <1.3`).
      if (partial.count < 3) {
        add_bound(VersionBound::Op::Ge, next_series(partial));
      } else {
        add_bound(VersionBound::Op::Gt, partial_to_version(partial));
      }
    } else if (op_text == ">=") {
      add_bound(VersionBound::Op::Ge, partial_to_version(partial));
    } else if (op_text == "<") {
      add_bound(VersionBound::Op::Lt, partial_to_version(partial));
    } else if (op_text == "<=") {
      // `<=1.2` includes the whole 1.2 series: `<=1.2.0` rejected 1.2.5.
      if (partial.count < 3) {
        add_bound(VersionBound::Op::Lt, next_series(partial));
      } else {
        add_bound(VersionBound::Op::Le, partial_to_version(partial));
      }
    } else if (op_text == "^") {
      // Cargo semantics: `^` allows changes that do not modify the left-most
      // non-zero component, so the upper bound depends on how many leading
      // components are zero — not on how many were written.
      //
      // The previous test (`base.major > 0 || partial.count > 1`) sent every
      // written-out version to `<major+1,0,0>`, making `^0.0.3` accept `0.9.0`
      // and `^0.1.2` accept `0.9.0`. The `else` branch that implemented the
      // 0.x rule was therefore unreachable.
      const Version base = partial_to_version(partial);
      add_bound(VersionBound::Op::Ge, base);
      if (base.major > 0 || partial.count == 1) {
        // ^1.2.3 -> <2.0.0 ; ^0 -> <1.0.0
        add_bound(VersionBound::Op::Lt, Version(base.major + 1, 0, 0));
      } else if (base.minor > 0 || partial.count == 2) {
        // ^0.2.3 -> <0.3.0 ; ^0.2 -> <0.3.0
        add_bound(VersionBound::Op::Lt, Version(0, base.minor + 1, 0));
      } else {
        // ^0.0.3 -> <0.0.4 ; ^0.0 -> <0.1.0 comes from the minor branch above
        add_bound(VersionBound::Op::Lt, Version(0, 0, base.patch + 1));
      }
    } else if (op_text == "~") {
      const Version base = partial_to_version(partial);
      add_bound(VersionBound::Op::Ge, base);
      if (partial.count >= 2) {
        add_bound(VersionBound::Op::Lt, Version(base.major, base.minor + 1, 0));
      } else {
        add_bound(VersionBound::Op::Lt, Version(base.major + 1, 0, 0));
      }
    } else {
      return std::unexpected(Error::make("cova::version::requirement", "unsupported version operator")
                      .with("clause: " + clause));
    }
  }

  if (requirement.bounds_.empty()) {
    return std::unexpected(Error::make("cova::version::requirement", "requirement has no bounds")
                    .with("input: " + std::string(trimmed)));
  }

  // Deterministic order for serialisation and comparison.
  std::stable_sort(requirement.bounds_.begin(), requirement.bounds_.end(),
                   [](const VersionBound& a, const VersionBound& b) {
                     const int order = compare_versions(a.version, b.version);
                     if (order != 0) {
                       return order < 0;
                     }
                     return static_cast<int>(a.op) < static_cast<int>(b.op);
                   });
  return requirement;
}

bool VersionRequirement::matches(const Version& candidate) const {
  if (any_) {
    return true;
  }
  for (const VersionBound& bound : bounds_) {
    if (!bound.matches(candidate)) {
      return false;
    }
  }
  return true;
}

bool VersionRequirement::is_any() const { return any_; }

bool VersionRequirement::is_exact() const { return exact_; }

std::optional<Version> VersionRequirement::exact_version() const {
  if (!exact_) {
    return std::nullopt;
  }
  for (const VersionBound& bound : bounds_) {
    if (bound.op == VersionBound::Op::Eq) {
      return bound.version;
    }
  }
  return std::nullopt;
}

std::string VersionRequirement::to_string() const {
  if (any_) {
    return "*";
  }
  std::vector<std::string> parts;
  parts.reserve(bounds_.size());
  for (const VersionBound& bound : bounds_) {
    parts.push_back(format_bound(bound));
  }
  return str::join(std::span<const std::string>(parts), ", ");
}

bool version_satisfies(std::string_view version_text, std::string_view requirement_text) {
  auto version = Version::parse(version_text);
  if (!version) {
    return false;
  }
  auto requirement = VersionRequirement::parse(requirement_text);
  if (!requirement) {
    return false;
  }
  return requirement->matches(*version);
}

}  // namespace cova
