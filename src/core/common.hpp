// Cova — common vocabulary types and string utilities.
//
// This header is part of the frozen architecture surface (see docs/ARCHITECTURE.md).
// Every layer may depend on it; it must not depend on any other Cova module.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cova {

// ---------------------------------------------------------------------------
// Error / Result
// ---------------------------------------------------------------------------

// A structured, machine-addressable error.
//
// `code` is a stable namespaced identifier such as "cova::target::unknown".
// `message` is a one-line, human-facing summary. `context` holds extra detail
// lines (e.g. "required", "found") rendered underneath by the diagnostic engine.
struct Error {
  std::string code;
  std::string message;
  std::vector<std::string> context;

  static Error make(std::string code, std::string message);
  // Appends a context line; returns *this so calls can chain.
  Error& with(std::string context_line);

  std::string to_string() const;
  bool operator==(const Error&) const = default;
};

template <class T>
using Result = std::expected<T, Error>;

using Status = std::expected<void, Error>;

inline Status ok() { return {}; }

// Convenience unwrap helpers.
inline Status fail(Error e) { return std::unexpected(std::move(e)); }

template <class T>
inline Result<T> fail(Error e) {
  return std::unexpected(std::move(e));
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
namespace str {

std::string_view trim_view(std::string_view s);
std::string trim(std::string_view s);

std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);

bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
bool iequals(std::string_view a, std::string_view b);
bool contains(std::string_view haystack, std::string_view needle);

std::vector<std::string> split(std::string_view s, char delimiter);
std::vector<std::string> split(std::string_view s, std::string_view delimiter);
// Splits on whitespace, honouring single and double quotes (used for command lines).
std::vector<std::string> split_whitespace_quoted(std::string_view s);

std::string join(std::span<const std::string> parts, std::string_view separator);
std::string join(std::span<const std::string_view> parts, std::string_view separator);

std::string replace_all(std::string_view s, std::string_view from, std::string_view to);

// Returns true when `s` only contains [A-Za-z0-9_-] and starts with a letter or '_'.
bool is_identifier(std::string_view s);
// Package names use the Cargo-like rule: lowercase alnum, '-', '_', no leading digit.
bool is_package_name(std::string_view s);

// Quote for shell-ish diagnostic display.
std::string quote(std::string_view s);
// Collapses runs of whitespace into single spaces and trims the result.
std::string collapse_whitespace(std::string_view s);

}  // namespace str

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Clamp `n` to [lo, hi].
template <class T>
constexpr T clamp_value(T n, T lo, T hi) {
  return n < lo ? lo : (hi < n ? hi : n);
}

// Deterministic hash of a string, used for short stable identifiers.
std::uint64_t fnv1a64(std::string_view s);

// Formats a byte count as a human readable string ("12.4 MiB").
std::string human_bytes(std::uint64_t bytes);

}  // namespace cova
