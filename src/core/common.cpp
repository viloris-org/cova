// Cova — common vocabulary types and string utilities (implementation).
#include "core/common.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace cova {

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------

Error Error::make(std::string code, std::string message) {
  Error error;
  error.code = std::move(code);
  error.message = std::move(message);
  return error;
}

Error& Error::with(std::string context_line) {
  context.push_back(std::move(context_line));
  return *this;
}

std::string Error::to_string() const {
  std::string out;
  out.reserve(code.size() + message.size() + 2);
  out += code;
  out += ": ";
  out += message;
  for (const std::string& context_line : context) {
    out += "\n  ";
    out += context_line;
  }
  return out;
}

// ---------------------------------------------------------------------------
// str
// ---------------------------------------------------------------------------
namespace str {
namespace {

constexpr bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

constexpr bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr char to_lower_char(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::string_view trim_view(std::string_view s) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end && is_space(s[begin])) {
    ++begin;
  }
  while (end > begin && is_space(s[end - 1])) {
    --end;
  }
  return s.substr(begin, end - begin);
}

std::string trim(std::string_view s) { return std::string(trim_view(s)); }

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = to_lower_char(c);
  }
  return out;
}

std::string to_upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (to_lower_char(a[i]) != to_lower_char(b[i])) {
      return false;
    }
  }
  return true;
}

bool contains(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  return haystack.find(needle) != std::string_view::npos;
}

std::vector<std::string> split(std::string_view s, char delimiter) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  for (;;) {
    const std::size_t pos = s.find(delimiter, begin);
    if (pos == std::string_view::npos) {
      parts.emplace_back(s.substr(begin));
      break;
    }
    parts.emplace_back(s.substr(begin, pos - begin));
    begin = pos + 1;
  }
  return parts;
}

std::vector<std::string> split(std::string_view s, std::string_view delimiter) {
  std::vector<std::string> parts;
  if (delimiter.empty()) {
    if (!s.empty()) {
      parts.emplace_back(s);
    }
    return parts;
  }
  std::size_t begin = 0;
  for (;;) {
    const std::size_t pos = s.find(delimiter, begin);
    if (pos == std::string_view::npos) {
      parts.emplace_back(s.substr(begin));
      break;
    }
    parts.emplace_back(s.substr(begin, pos - begin));
    begin = pos + delimiter.size();
  }
  return parts;
}

std::vector<std::string> split_whitespace_quoted(std::string_view s) {
  std::vector<std::string> parts;
  std::string current;
  bool in_single = false;
  bool in_double = false;
  bool has_token = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (in_single) {
      if (c == '\'') {
        in_single = false;
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (in_double) {
      if (c == '\\' && i + 1 < s.size()) {
        current.push_back(s[++i]);
      } else if (c == '"') {
        in_double = false;
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (c == '\'') {
      in_single = true;
      has_token = true;
      continue;
    }
    if (c == '"') {
      in_double = true;
      has_token = true;
      continue;
    }
    if (is_space(c)) {
      if (has_token || !current.empty()) {
        parts.push_back(current);
        current.clear();
        has_token = false;
      }
      continue;
    }
    current.push_back(c);
    has_token = true;
  }
  if (has_token || !current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

std::string join(std::span<const std::string> parts, std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out += separator;
    }
    out += parts[i];
  }
  return out;
}

std::string join(std::span<const std::string_view> parts, std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out += separator;
    }
    out += parts[i];
  }
  return out;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
  if (from.empty()) {
    return std::string(s);
  }
  std::string out;
  out.reserve(s.size());
  std::size_t begin = 0;
  for (;;) {
    const std::size_t pos = s.find(from, begin);
    if (pos == std::string_view::npos) {
      out.append(s.substr(begin));
      break;
    }
    out.append(s.substr(begin, pos - begin));
    out.append(to);
    begin = pos + from.size();
  }
  return out;
}

bool is_identifier(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  if (!(is_alpha(s[0]) || s[0] == '_')) {
    return false;
  }
  for (char c : s) {
    if (!(is_alpha(c) || is_digit(c) || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool is_package_name(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  auto lower_alpha = [](char c) { return c >= 'a' && c <= 'z'; };
  // The first character must be a letter or '_'. A leading '-' would make the
  // name (and the artifact derived from it) look like a command-line option,
  // and names such as "-bad" or "--" also produced unusable directories.
  if (!lower_alpha(s[0]) && s[0] != '_') {
    return false;
  }
  for (char c : s) {
    if (!(lower_alpha(c) || is_digit(c) || c == '-' || c == '_')) {
      return false;
    }
  }
  // No trailing separator, and no doubled separators: they make awkward
  // artifact names and are almost always a typo.
  if (s.back() == '-' || s.back() == '_') {
    return false;
  }
  for (std::size_t i = 0; i + 1 < s.size(); ++i) {
    const char a = s[i];
    const char b = s[i + 1];
    if ((a == '-' || a == '_') && a == b) {
      return false;
    }
  }
  return true;
}

std::string quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"':
      case '\\':
        out.push_back('\\');
        out.push_back(c);
        continue;
      case '\n':
        out += "\\n";
        continue;
      case '\r':
        out += "\\r";
        continue;
      case '\t':
        out += "\\t";
        continue;
      default:
        break;
    }
    // Control characters would break a single-line diagnostic (or, worse, emit
    // terminal escape sequences from an untrusted manifest value).
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      static constexpr char kHex[] = "0123456789abcdef";
      out += "\\x";
      out.push_back(kHex[(byte >> 4) & 0xF]);
      out.push_back(kHex[byte & 0xF]);
      continue;
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string collapse_whitespace(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;
  for (char c : s) {
    if (is_space(c)) {
      if (!out.empty()) {
        pending_space = true;
      }
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace str

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::uint64_t fnv1a64(std::string_view s) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char byte : s) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string human_bytes(std::uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  if (bytes < 1024) {
    return std::to_string(bytes) + " B";
  }
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, kUnits[unit]);
  return std::string(buffer);
}

}  // namespace cova
