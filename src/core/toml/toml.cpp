// Cova — TOML reader/writer (implementation).
//
// Hand-written for the subset documented in toml.hpp. Every parse error carries
// a stable "cova::toml::parse" code and 1-based line/column context so manifest
// diagnostics can point at the offending line (PRD §19.3).
#include "core/toml/toml.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common.hpp"
#include "core/filesystem/filesystem.hpp"

namespace cova::toml {
namespace {

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr bool is_hex_digit(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr bool is_bare_key_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' || c == '-';
}

constexpr int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

std::string render_string(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

bool is_bare_key(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  for (char c : s) {
    if (!is_bare_key_char(c)) {
      return false;
    }
  }
  return true;
}

std::string render_key(std::string_view key) {
  return is_bare_key(key) ? std::string(key) : render_string(key);
}

std::string render_float(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value < 0 ? "-inf" : "inf";
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec == std::errc()) {
    std::string text(buffer, result.ptr);
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
      text += ".0";
    }
    return text;
  }
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return std::string(buffer);
}

std::string join_path(const std::vector<std::string>& path) {
  std::string out;
  for (const std::string& component : path) {
    if (!out.empty()) {
      out.push_back('.');
    }
    out += render_key(component);
  }
  return out;
}

std::string render_inline(const Value& value);

std::string render_array(const Array& array) {
  std::string out = "[";
  for (std::size_t i = 0; i < array.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += render_inline(array[i]);
  }
  out.push_back(']');
  return out;
}

std::string render_inline_table(const Table& table) {
  std::string out = "{ ";
  bool first = true;
  for (const auto& [key, value] : table) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += render_key(key);
    out += " = ";
    out += render_inline(value);
  }
  out += " }";
  return out;
}

std::string render_inline(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::Invalid:
      return "";
    case Value::Kind::Boolean:
      return value.as_bool() ? "true" : "false";
    case Value::Kind::Integer:
      return std::to_string(value.as_integer());
    case Value::Kind::Float:
      return render_float(value.as_float());
    case Value::Kind::String:
      return render_string(value.as_string());
    case Value::Kind::Datetime:
      return value.as_string();
    case Value::Kind::Array:
      return render_array(*value.as_array());
    case Value::Kind::Table:
      return render_inline_table(*value.as_table());
  }
  return "";
}

bool is_table_array(const Value& value) {
  const Array* array = value.as_array();
  if (array == nullptr || array->empty()) {
    return false;
  }
  return std::all_of(array->begin(), array->end(),
                     [](const Value& entry) { return entry.is_table(); });
}

void render_table(const Table& table, const std::vector<std::string>& prefix, std::string& out) {
  // Scalar-ish keys first, in sorted order for determinism.
  for (const auto& [key, value] : table) {
    if (value.is_table() || is_table_array(value)) {
      continue;
    }
    out += render_key(key);
    out += " = ";
    out += render_inline(value);
    out += "\n";
  }

  for (const auto& [key, value] : table) {
    if (value.is_table()) {
      if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
      }
      std::vector<std::string> path = prefix;
      path.push_back(key);
      out += "[";
      out += join_path(path);
      out += "]\n";
      render_table(*value.as_table(), path, out);
      out += "\n";
      continue;
    }
    if (!is_table_array(value)) {
      continue;
    }
    std::vector<std::string> path = prefix;
    path.push_back(key);
    for (const Value& entry : *value.as_array()) {
      if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
      }
      out += "[[";
      out += join_path(path);
      out += "]]\n";
      render_table(*entry.as_table(), path, out);
      out += "\n";
    }
  }

  // Collapse the trailing blank line produced by the last sub-table.
  while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') {
    out.pop_back();
  }
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Validates the shapes TOML 1.0 allows for offset date-times, local
// date-times, local dates and local times.
//
// Field widths are fixed by RFC 3339 (4-2-2 date, 2:2:2 time, optional
// fractional seconds), which is what makes `1979-05-27T99:99:99` invalid rather
// than "an unusual but acceptable value".
bool valid_datetime_shapes(std::string_view raw) {
  auto digits_at = [raw](std::size_t pos, std::size_t count) {
    if (pos + count > raw.size()) {
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (!is_digit(raw[pos + i])) {
        return false;
      }
    }
    return true;
  };
  auto in_range = [raw](std::size_t pos, std::size_t count, int low, int high) {
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      value = value * 10 + (raw[pos + i] - '0');
    }
    return value >= low && value <= high;
  };

  std::size_t pos = 0;
  bool saw_date = false;
  bool saw_time = false;

  // Date: YYYY-MM-DD
  if (digits_at(0, 4) && raw.size() >= 10 && raw[4] == '-' && raw[7] == '-' && digits_at(5, 2) &&
      digits_at(8, 2)) {
    if (!in_range(5, 2, 1, 12) || !in_range(8, 2, 1, 31) || !in_range(0, 4, 0, 9999)) {
      return false;
    }
    pos = 10;
    saw_date = true;
  }

  // Time: HH:MM:SS[.fraction]
  auto parse_time = [&](std::size_t start) {
    if (raw.size() < start + 8 || !digits_at(start, 2) || raw[start + 2] != ':' ||
        !digits_at(start + 3, 2) || raw[start + 5] != ':' || !digits_at(start + 6, 2)) {
      return std::optional<std::size_t>{};
    }
    if (!in_range(start, 2, 0, 23) || !in_range(start + 3, 2, 0, 59) ||
        !in_range(start + 6, 2, 0, 60)) {  // 60 allows a leap second
      return std::optional<std::size_t>{};
    }
    std::size_t next = start + 8;
    if (next < raw.size() && raw[next] == '.') {
      std::size_t fraction = next + 1;
      while (fraction < raw.size() && is_digit(raw[fraction])) {
        ++fraction;
      }
      if (fraction == next + 1) {
        return std::optional<std::size_t>{};
      }
      next = fraction;
    }
    return std::optional<std::size_t>{next};
  };

  if (saw_date && pos < raw.size()) {
    // A space separator is only valid when a time follows.
    if (raw[pos] == 'T' || raw[pos] == 't' || raw[pos] == ' ') {
      auto next = parse_time(pos + 1);
      if (!next) {
        return false;
      }
      pos = *next;
      saw_time = true;
    }
  } else if (!saw_date) {
    auto next = parse_time(0);
    if (!next) {
      return false;
    }
    pos = *next;
    saw_time = true;
  }

  if (!saw_date && !saw_time) {
    return false;
  }

  // Optional offset: Z, or +/-HH:MM.
  if (pos < raw.size()) {
    if (raw[pos] == 'Z' || raw[pos] == 'z') {
      if (pos + 1 != raw.size()) {
        return false;
      }
      return true;
    }
    if (raw[pos] == '+' || raw[pos] == '-') {
      if (pos + 6 != raw.size() || !digits_at(pos + 1, 2) || raw[pos + 3] != ':' ||
          !digits_at(pos + 4, 2)) {
        return false;
      }
      return in_range(pos + 1, 2, 0, 23) && in_range(pos + 4, 2, 0, 59);
    }
    return false;
  }
  return true;
}

// Maximum nesting depth for arrays and inline tables. Chosen to be far beyond
// any real manifest or lockfile (which nest a handful of levels) while staying
// well inside the smallest stack Cova runs on. Matches `json.cpp`'s bound.
constexpr std::uint32_t kMaxNestingDepth = 128;

class Parser {
 public:
  Parser(std::string_view text, std::string_view source) : text_(text), source_(source) {}

  Result<Value> run();

 private:
  // --- position helpers ----------------------------------------------------
  bool at_end() const { return pos_ >= text_.size(); }
  char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  char peek_at(std::size_t offset) const {
    return pos_ + offset < text_.size() ? text_[pos_ + offset] : '\0';
  }
  bool starts_with(std::string_view token) const {
    return text_.substr(pos_, token.size()) == token;
  }
  void bump() {
    if (pos_ >= text_.size()) {
      return;
    }
    if (text_[pos_] == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    ++pos_;
  }
  void bump(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      bump();
    }
  }
  void skip_spaces() {
    while (!at_end() && (peek() == ' ' || peek() == '\t')) {
      bump();
    }
  }
  void skip_comment() {
    if (peek() != '#') {
      return;
    }
    while (!at_end() && peek() != '\n') {
      bump();
    }
  }
  void skip_blank_and_comments() {
    for (;;) {
      skip_spaces();
      if (peek() == '#') {
        skip_comment();
      }
      if (peek() == '\r' && peek_at(1) == '\n') {
        bump();
        bump();
        continue;
      }
      if (peek() == '\n') {
        bump();
        continue;
      }
      break;
    }
  }
  bool consume(char expected) {
    if (peek() == expected) {
      bump();
      return true;
    }
    return false;
  }
  void expect_newline_or_end() {
    // Caller has already skipped spaces; a comment then newline is allowed.
    if (peek() == '#') {
      skip_comment();
    }
    if (peek() == '\r' && peek_at(1) == '\n') {
      bump();
      bump();
      return;
    }
    if (peek() == '\n' || at_end()) {
      if (peek() == '\n') {
        bump();
      }
      return;
    }
    throw_error("unexpected trailing characters after value");
  }

  // --- errors --------------------------------------------------------------
  [[noreturn]] void throw_error(std::string message) {
    throw ParseAbort{make_error(std::move(message))};
  }

  Error make_error(std::string message) const {
    Error error = Error::make("cova::toml::parse", std::move(message));
    error.with("source: " + std::string(source_));
    error.with("line: " + std::to_string(line_));
    error.with("column: " + std::to_string(column_));
    return error;
  }

  SourceLocation location() const {
    return SourceLocation{static_cast<std::uint32_t>(line_), static_cast<std::uint32_t>(column_)};
  }

  struct ParseAbort {
    Error error;
  };

  // --- grammar -------------------------------------------------------------
  void parse_document_into();
  void parse_header();
  void parse_key_value();
  std::vector<std::string> parse_dotted_key();
  std::string parse_single_key();
  Value parse_value();
  std::string parse_basic_string(bool multiline);
  std::string parse_literal_string(bool multiline);
  Value parse_number_or_datetime();
  Value parse_array();
  Value parse_inline_table();
  void append_utf8(std::string& out, std::uint32_t code_point);

  Table* open_table(Table* base, const std::vector<std::string>& path);
  Table* current_table();
  void assign(const std::vector<std::string>& key_path, Value value);

  std::string_view text_;
  std::string_view source_;
  std::size_t pos_ = 0;
  std::uint32_t line_ = 1;
  std::uint32_t column_ = 1;
  std::shared_ptr<Table> root_ = std::make_shared<Table>();
  Table* current_ = root_.get();
  std::vector<std::string> current_path_;
  // Fully-qualified headers already declared, to reject duplicate [table]s.
  std::map<std::string, bool, std::less<>> defined_tables_;
  // Current value nesting depth (arrays + inline tables). Bounded by
  // `kMaxNestingDepth` so recursion cannot overflow the stack.
  std::uint32_t depth_ = 0;
};

void Parser::append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7Fu) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point <= 0x10FFFFu) {
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else {
    throw_error("unicode escape out of range");
  }
}

std::string Parser::parse_basic_string(bool multiline) {
  std::string out;
  if (multiline) {
    bump(3);  // opening """
    // A newline immediately after the opening delimiter is trimmed.
    if (peek() == '\r' && peek_at(1) == '\n') {
      bump(2);
    } else if (peek() == '\n') {
      bump();
    }
    for (;;) {
      if (at_end()) {
        throw_error("unterminated multi-line basic string");
      }
      if (starts_with("\"\"\"")) {
        // TOML allows up to two extra quotes directly before the closing
        // delimiter; those belong to the string content.
        std::size_t quotes = 3;
        while (peek_at(quotes) == '"' && quotes < 5) {
          ++quotes;
        }
        out.append(quotes - 3, '"');
        bump(quotes);
        return out;
      }
      const char c = peek();
      if (c == '\\') {
        bump();
        if (at_end()) {
          throw_error("unterminated escape sequence");
        }
        const char escape = peek();
        // A backslash at the end of a line trims the following whitespace.
        if (escape == '\n' || escape == '\r' || escape == ' ' || escape == '\t') {
          while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            bump();
          }
          continue;
        }
        switch (escape) {
          case 'b':
            out.push_back('\b');
            bump();
            break;
          case 't':
            out.push_back('\t');
            bump();
            break;
          case 'n':
            out.push_back('\n');
            bump();
            break;
          case 'f':
            out.push_back('\f');
            bump();
            break;
          case 'r':
            out.push_back('\r');
            bump();
            break;
          case '"':
            out.push_back('"');
            bump();
            break;
          case '\\':
            out.push_back('\\');
            bump();
            break;
          case 'u':
          case 'U': {
            const int digits = escape == 'u' ? 4 : 8;
            bump();
            std::uint32_t code_point = 0;
            for (int i = 0; i < digits; ++i) {
              if (!is_hex_digit(peek())) {
                throw_error("invalid unicode escape");
              }
              code_point = code_point * 16 + static_cast<std::uint32_t>(hex_value(peek()));
              bump();
            }
            append_utf8(out, code_point);
            break;
          }
          default:
            throw_error("invalid escape sequence in basic string");
        }
        continue;
      }
      if (c == '\n' || c == '\r') {
        if (!multiline) {
          throw_error("newline in single-line basic string");
        }
      }
      out.push_back(c);
      bump();
    }
  }

  bump();  // opening quote
  for (;;) {
    if (at_end()) {
      throw_error("unterminated basic string");
    }
    const char c = peek();
    if (c == '"') {
      bump();
      return out;
    }
    if (c == '\n' || c == '\r') {
      throw_error("newline in single-line basic string");
    }
    if (c == '\\') {
      bump();
      if (at_end()) {
        throw_error("unterminated escape sequence");
      }
      switch (peek()) {
        case 'b':
          out.push_back('\b');
          bump();
          break;
        case 't':
          out.push_back('\t');
          bump();
          break;
        case 'n':
          out.push_back('\n');
          bump();
          break;
        case 'f':
          out.push_back('\f');
          bump();
          break;
        case 'r':
          out.push_back('\r');
          bump();
          break;
        case '"':
          out.push_back('"');
          bump();
          break;
        case '\\':
          out.push_back('\\');
          bump();
          break;
        case 'u':
        case 'U': {
          const int digits = peek() == 'u' ? 4 : 8;
          bump();
          std::uint32_t code_point = 0;
          for (int i = 0; i < digits; ++i) {
            if (!is_hex_digit(peek())) {
              throw_error("invalid unicode escape");
            }
            code_point = code_point * 16 + static_cast<std::uint32_t>(hex_value(peek()));
            bump();
          }
          append_utf8(out, code_point);
          break;
        }
        default:
          throw_error("invalid escape sequence in basic string");
      }
      continue;
    }
    out.push_back(c);
    bump();
  }
}

std::string Parser::parse_literal_string(bool multiline) {
  std::string out;
  if (multiline) {
    bump(3);
    if (peek() == '\r' && peek_at(1) == '\n') {
      bump(2);
    } else if (peek() == '\n') {
      bump();
    }
    for (;;) {
      if (at_end()) {
        throw_error("unterminated multi-line literal string");
      }
      if (starts_with("'''")) {
        std::size_t quotes = 3;
        while (peek_at(quotes) == '\'' && quotes < 5) {
          ++quotes;
        }
        out.append(quotes - 3, '\'');
        bump(quotes);
        return out;
      }
      out.push_back(peek());
      bump();
    }
  }
  bump();  // opening quote
  for (;;) {
    if (at_end()) {
      throw_error("unterminated literal string");
    }
    const char c = peek();
    if (c == '\'') {
      bump();
      return out;
    }
    if (c == '\n' || c == '\r') {
      throw_error("newline in single-line literal string");
    }
    out.push_back(c);
    bump();
  }
}

std::string Parser::parse_single_key() {
  if (peek() == '"') {
    if (peek_at(1) == '"' && peek_at(2) == '"') {
      throw_error("multi-line strings are not allowed as keys");
    }
    return parse_basic_string(false);
  }
  if (peek() == '\'') {
    if (peek_at(1) == '\'' && peek_at(2) == '\'') {
      throw_error("multi-line strings are not allowed as keys");
    }
    return parse_literal_string(false);
  }
  const std::size_t begin = pos_;
  while (!at_end() && is_bare_key_char(peek())) {
    bump();
  }
  if (pos_ == begin) {
    throw_error("expected a key");
  }
  return std::string(text_.substr(begin, pos_ - begin));
}

std::vector<std::string> Parser::parse_dotted_key() {
  std::vector<std::string> path;
  path.push_back(parse_single_key());
  for (;;) {
    skip_spaces();
    if (peek() != '.') {
      break;
    }
    bump();
    skip_spaces();
    path.push_back(parse_single_key());
  }
  return path;
}

Value Parser::parse_number_or_datetime() {
  // Datetimes must be recognised before numbers: 1979-05-27 starts like an int.
  auto looks_like_date = [this]() {
    return is_digit(peek()) && is_digit(peek_at(1)) && is_digit(peek_at(2)) && is_digit(peek_at(3)) &&
           peek_at(4) == '-';
  };
  auto looks_like_time = [this]() {
    return is_digit(peek()) && is_digit(peek_at(1)) && peek_at(2) == ':';
  };
  if (looks_like_date() || looks_like_time()) {
    auto is_datetime_char = [](char c) {
      return is_digit(c) || c == '-' || c == ':' || c == '.' || c == 'T' || c == 't' || c == 'Z' ||
             c == 'z' || c == '+' || c == ' ';
    };
    std::string raw;
    // A space is only part of a datetime when followed by a time.
    while (!at_end() && is_datetime_char(peek())) {
      if (peek() == ' ') {
        if (!(is_digit(peek_at(1)) && is_digit(peek_at(2)) && peek_at(3) == ':')) {
          break;
        }
      }
      raw.push_back(peek());
      bump();
    }
    // There must be a date or time shape; otherwise it was a number like 2024.
    const bool looks_like_a_date = raw.size() >= 8 && raw.find('-') != std::string::npos;
    const bool looks_like_a_time = raw.size() >= 8 && raw.find(':') != std::string::npos;
    if (!looks_like_a_date && !looks_like_a_time) {
      throw_error("malformed date-time value");
    }
    // The character scan above accepts any run of digits and separators, which
    // let `1979-05-27T99:99:99` and `1979-05-27T07:32:00ZZ` through. TOML 1.0
    // requires RFC 3339 with fixed field widths; a manifest with a typo'd date
    // must be rejected here, not silently stored.
    if (!valid_datetime_shapes(raw)) {
      throw_error("malformed date-time value `" + raw + "`");
    }
    return Value::datetime(std::move(raw));
  }

  const bool negative = consume('-');
  if (!negative) {
    consume('+');
  }
  if (starts_with("inf") || starts_with("nan")) {
    const bool is_nan = starts_with("nan");
    bump(3);
    if (is_nan) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
    return Value(negative ? -std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::infinity());
  }
  // Hex / octal / binary integers.
  if (peek() == '0' && (peek_at(1) == 'x' || peek_at(1) == 'X' || peek_at(1) == 'o' ||
                        peek_at(1) == 'O' || peek_at(1) == 'b' || peek_at(1) == 'B')) {
    if (negative) {
      throw_error("negative hexadecimal/octal/binary integers are not supported by TOML");
    }
    const char marker = peek_at(1);
    const int base = (marker == 'x' || marker == 'X') ? 16
                     : (marker == 'o' || marker == 'O') ? 8
                                                        : 2;
    bump(2);
    const std::size_t digits_begin = pos_;
    std::string digits;
    bool last_was_underscore = false;
    while (!at_end()) {
      const char c = peek();
      if (c == '_') {
        if (last_was_underscore || pos_ == digits_begin) {
          throw_error("invalid underscore in integer");
        }
        last_was_underscore = true;
        bump();
        continue;
      }
      if (!is_hex_digit(c) || (base != 16 && hex_value(c) >= base)) {
        break;
      }
      digits.push_back(c);
      last_was_underscore = false;
      bump();
    }
    if (digits.empty() || last_was_underscore) {
      throw_error("expected digits after integer base prefix");
    }
    std::int64_t value = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (result.ec != std::errc() || result.ptr != digits.data() + digits.size()) {
      throw_error("integer literal out of range");
    }
    return Value(value);
  }

  // Decimal integer or float.
  const std::size_t digits_begin = pos_;
  std::string token;
  bool last_was_underscore = false;
  bool saw_digit = false;
  while (!at_end()) {
    const char c = peek();
    if (is_digit(c)) {
      token.push_back(c);
      saw_digit = true;
      last_was_underscore = false;
      bump();
      continue;
    }
    if (c == '_') {
      if (!saw_digit || last_was_underscore) {
        throw_error("invalid underscore in number");
      }
      last_was_underscore = true;
      bump();
      continue;
    }
    break;
  }
  if (!saw_digit) {
    throw_error("expected a number");
  }
  if (last_was_underscore) {
    throw_error("trailing underscore in number");
  }
  if (token.size() > 1 && token[0] == '0') {
    throw_error("leading zeros are not allowed in integers");
  }
  // Reject a second integer-looking component without an operator (e.g. "1 2").
  bool is_float = false;
  if (peek() == '.') {
    is_float = true;
    token.push_back('.');
    bump();
    if (!is_digit(peek())) {
      throw_error("expected digits after decimal point");
    }
    while (!at_end()) {
      const char c = peek();
      if (is_digit(c)) {
        token.push_back(c);
        bump();
        continue;
      }
      if (c == '_') {
        if (!is_digit(peek_at(1))) {
          throw_error("invalid underscore in number");
        }
        bump();
        continue;
      }
      break;
    }
  }
  if (peek() == 'e' || peek() == 'E') {
    is_float = true;
    token.push_back(peek());
    bump();
    if (peek() == '+' || peek() == '-') {
      token.push_back(peek());
      bump();
    }
    if (!is_digit(peek())) {
      throw_error("expected digits in exponent");
    }
    while (!at_end() && (is_digit(peek()) || (peek() == '_' && is_digit(peek_at(1))))) {
      if (peek() != '_') {
        token.push_back(peek());
      }
      bump();
    }
  }
  (void)digits_begin;
  if (is_float) {
    if (negative) {
      token.insert(token.begin(), '-');
    }
    double value = 0.0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc()) {
      throw_error("floating point literal out of range");
    }
    return Value(value);
  }
  if (negative) {
    token.insert(token.begin(), '-');
  }
  std::int64_t value = 0;
  const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
  if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
    throw_error("integer literal out of range");
  }
  return Value(value);
}

Value Parser::parse_array() {
  bump();  // '['
  Array array;
  for (;;) {
    skip_blank_and_comments();
    if (at_end()) {
      throw_error("unterminated array");
    }
    if (consume(']')) {
      return Value(std::move(array));
    }
    array.push_back(parse_value());
    skip_blank_and_comments();
    if (consume(',')) {
      continue;
    }
    if (consume(']')) {
      return Value(std::move(array));
    }
    throw_error("expected ',' or ']' in array");
  }
}

Value Parser::parse_inline_table() {
  bump();  // '{'
  Table table;
  skip_spaces();
  if (consume('}')) {
    return Value(std::move(table));
  }
  for (;;) {
    skip_spaces();
    const std::vector<std::string> key_path = parse_dotted_key();
    skip_spaces();
    if (!consume('=')) {
      throw_error("expected '=' in inline table");
    }
    skip_spaces();
    // Insert respecting dotted keys within the inline table.
    Table* target = &table;
    for (std::size_t i = 0; i + 1 < key_path.size(); ++i) {
      auto it = target->find(key_path[i]);
      if (it == target->end()) {
        auto [inserted, _] = target->emplace(key_path[i], Value(Table{}));
        it = inserted;
      }
      if (!it->second.is_table()) {
        throw_error("inline table key conflicts with an existing value");
      }
      target = it->second.as_table();
    }
    if (target->find(key_path.back()) != target->end()) {
      throw_error("duplicate key in inline table");
    }
    target->emplace(key_path.back(), parse_value());
    skip_spaces();
    if (consume(',')) {
      continue;
    }
    if (consume('}')) {
      return Value(std::move(table));
    }
    throw_error("expected ',' or '}' in inline table");
  }
}

Value Parser::parse_value() {
  // Guard against unbounded recursion through nested arrays and inline tables.
  //
  // `parse_value` -> `parse_array` -> `parse_value` is mutual recursion whose
  // depth is controlled entirely by the input. Without a limit a hostile or
  // merely corrupt document (`a = [[[[...]]]]`) overflows the stack and kills
  // the process instead of reporting a parse error (PRD §19.3 requires every
  // parse error to carry a location). `json.cpp` has enforced the same bound
  // since it was written; TOML did not.
  //
  // The RAII guard is essential: `throw_error` throws, and the depth must be
  // unwound on that path too or a later sibling value would inherit the depth.
  struct DepthGuard {
    std::uint32_t& depth;
    ~DepthGuard() { --depth; }
  } depth_guard{depth_};
  if (++depth_ > kMaxNestingDepth) {
    throw_error("nesting is too deep (limit " + std::to_string(kMaxNestingDepth) + ")");
  }

  const SourceLocation start = location();
  Value value;
  switch (peek()) {
    case '"':
      if (peek_at(1) == '"' && peek_at(2) == '"') {
        value = Value(parse_basic_string(true));
      } else {
        value = Value(parse_basic_string(false));
      }
      break;
    case '\'':
      if (peek_at(1) == '\'' && peek_at(2) == '\'') {
        value = Value(parse_literal_string(true));
      } else {
        value = Value(parse_literal_string(false));
      }
      break;
    case '[':
      value = parse_array();
      break;
    case '{':
      value = parse_inline_table();
      break;
    case 't':
      if (!starts_with("true")) {
        throw_error("invalid value (expected true/false)");
      }
      bump(4);
      value = Value(true);
      break;
    case 'f':
      if (!starts_with("false")) {
        throw_error("invalid value (expected true/false)");
      }
      bump(5);
      value = Value(false);
      break;
    default:
      value = parse_number_or_datetime();
      break;
  }
  value.set_location(start);
  return value;
}

Table* Parser::open_table(Table* base, const std::vector<std::string>& path) {
  Table* table = base;
  for (const std::string& component : path) {
    auto it = table->find(component);
    if (it == table->end()) {
      auto [inserted, _] = table->emplace(component, Value(Table{}));
      it = inserted;
    }
    if (it->second.is_table()) {
      table = it->second.as_table();
      continue;
    }
    if (it->second.is_array()) {
      Array* array = it->second.as_array();
      if (!array->empty() && array->back().is_table()) {
        table = array->back().as_table();
        continue;
      }
      throw_error("cannot extend an array that is not an array of tables");
    }
    throw_error("key is already defined as a non-table value");
  }
  return table;
}

Table* Parser::current_table() { return current_; }

void Parser::assign(const std::vector<std::string>& key_path, Value value) {
  Table* table = current_table();
  for (std::size_t i = 0; i + 1 < key_path.size(); ++i) {
    auto it = table->find(key_path[i]);
    if (it == table->end()) {
      auto [inserted, _] = table->emplace(key_path[i], Value(Table{}));
      it = inserted;
    }
    if (!it->second.is_table()) {
      throw_error("dotted key conflicts with an existing value");
    }
    table = it->second.as_table();
  }
  if (table->find(key_path.back()) != table->end()) {
    throw_error("duplicate key '" + key_path.back() + "'");
  }
  table->emplace(key_path.back(), std::move(value));
}

void Parser::parse_header() {
  const std::size_t header_begin = pos_;
  bump();  // '['
  const bool is_array = consume('[');
  skip_spaces();
  std::vector<std::string> path = parse_dotted_key();
  skip_spaces();
  if (!consume(']')) {
    throw_error("expected ']' to close a table header");
  }
  if (is_array && !consume(']')) {
    throw_error("expected ']]' to close an array-of-tables header");
  }
  (void)header_begin;

  const std::string joined = join_path(path);
  const std::string key = joined;

  if (is_array) {
    std::vector<std::string> parent_path(path.begin(), path.end() - 1);
    Table* parent = open_table(root_.get(), parent_path);
    auto it = parent->find(path.back());
    if (it == parent->end()) {
      auto [inserted, _] = parent->emplace(path.back(), Value(Array{}));
      it = inserted;
    } else if (!it->second.is_array()) {
      throw_error("[" + key + "] is already defined as a non-array value");
    }
    it->second.as_array()->push_back(Value(Table{}));
    current_ = it->second.as_array()->back().as_table();
    current_path_ = path;
    return;
  }

  if (defined_tables_.find(key) != defined_tables_.end()) {
    throw_error("table [" + key + "] is defined more than once");
  }
  current_ = open_table(root_.get(), path);
  current_path_ = path;
  defined_tables_.emplace(key, true);
}

void Parser::parse_key_value() {
  const std::vector<std::string> key_path = parse_dotted_key();
  skip_spaces();
  if (!consume('=')) {
    throw_error("expected '=' after key");
  }
  skip_spaces();
  Value value = parse_value();
  assign(key_path, std::move(value));
}

void Parser::parse_document_into() {
  // Skip a UTF-8 BOM if present.
  if (starts_with("\xEF\xBB\xBF")) {
    bump(3);
  }
  for (;;) {
    skip_blank_and_comments();
    if (at_end()) {
      break;
    }
    if (peek() == '[') {
      parse_header();
    } else {
      parse_key_value();
    }
    skip_spaces();
    expect_newline_or_end();
  }
}

Result<Value> Parser::run() {
  try {
    parse_document_into();
  } catch (const ParseAbort& abort) {
    return std::unexpected(abort.error);
  } catch (const std::exception& exception) {
    return std::unexpected(make_error(std::string("internal parse failure: ") + exception.what()));
  }
  Value root{Table(*root_)};
  root.set_location(SourceLocation{1, 1});
  return root;
}

}  // namespace

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

Value::Value() = default;

Value::Value(bool v) : kind_(Kind::Boolean), bool_(v) {}

Value::Value(std::int64_t v) : kind_(Kind::Integer), int_(v) {}

Value::Value(double v) : kind_(Kind::Float), float_(v) {}

Value::Value(std::string v) : kind_(Kind::String), string_(std::move(v)) {}

Value::Value(Array v) : kind_(Kind::Array), array_(std::move(v)) {}

Value::Value(Table v) : kind_(Kind::Table), table_(std::make_shared<Table>(std::move(v))) {}

Value Value::string_table() { return Value(Table{}); }

Value Value::datetime(std::string raw) {
  Value value;
  value.kind_ = Kind::Datetime;
  value.string_ = std::move(raw);
  return value;
}

bool Value::as_bool(bool fallback) const { return kind_ == Kind::Boolean ? bool_ : fallback; }

std::int64_t Value::as_integer(std::int64_t fallback) const {
  if (kind_ == Kind::Integer) {
    return int_;
  }
  if (kind_ == Kind::Float) {
    return static_cast<std::int64_t>(float_);
  }
  return fallback;
}

double Value::as_float(double fallback) const {
  if (kind_ == Kind::Float) {
    return float_;
  }
  if (kind_ == Kind::Integer) {
    return static_cast<double>(int_);
  }
  return fallback;
}

std::string Value::as_string(std::string_view fallback) const {
  if (kind_ == Kind::String || kind_ == Kind::Datetime) {
    return string_;
  }
  return std::string(fallback);
}

const Array* Value::as_array() const { return kind_ == Kind::Array ? &array_ : nullptr; }
const Table* Value::as_table() const { return kind_ == Kind::Table ? table_.get() : nullptr; }
Array* Value::as_array() { return kind_ == Kind::Array ? &array_ : nullptr; }
Table* Value::as_table() { return kind_ == Kind::Table ? table_.get() : nullptr; }

std::vector<std::string> Value::as_string_array() const {
  std::vector<std::string> out;
  if (kind_ == Kind::String || kind_ == Kind::Datetime) {
    out.push_back(string_);
    return out;
  }
  if (kind_ != Kind::Array) {
    return out;
  }
  out.reserve(array_.size());
  for (const Value& entry : array_) {
    if (entry.is_string() || entry.kind_ == Kind::Datetime) {
      out.push_back(entry.string_);
    }
  }
  return out;
}

std::string Value::type_name() const {
  switch (kind_) {
    case Kind::Invalid:
      return "invalid";
    case Kind::Boolean:
      return "boolean";
    case Kind::Integer:
      return "integer";
    case Kind::Float:
      return "float";
    case Kind::String:
      return "string";
    case Kind::Array:
      return "array";
    case Kind::Table:
      return "table";
    case Kind::Datetime:
      return "datetime";
  }
  return "invalid";
}

std::string Value::to_toml(int indent) const {
  const std::string pad(static_cast<std::size_t>(indent), ' ');
  switch (kind_) {
    case Kind::Invalid:
      return "";
    case Kind::Boolean:
      return bool_ ? "true" : "false";
    case Kind::Integer:
      return std::to_string(int_);
    case Kind::Float:
      return render_float(float_);
    case Kind::String:
      return render_string(string_);
    case Kind::Datetime:
      return string_;
    case Kind::Array:
      return render_array(array_);
    case Kind::Table:
      return render_inline_table(*table_);
  }
  (void)pad;
  return "";
}

// ---------------------------------------------------------------------------
// Document API
// ---------------------------------------------------------------------------

Result<Value> parse(std::string_view text, std::string_view source_name) {
  return Parser(text, source_name).run();
}

Result<Value> parse_file(const std::string& path) {
  auto contents = fs::read_file(fs::Path(path));
  if (!contents) {
    Error error = std::move(contents.error());
    error.with("source: " + path);
    return std::unexpected(std::move(error));
  }
  return parse(*contents, path);
}

std::string write(const Value& value) {
  if (const Table* table = value.as_table(); table != nullptr) {
    return write(*table);
  }
  std::string out = render_inline(value);
  out.push_back('\n');
  return out;
}

std::string write(const Table& table) {
  std::string out;
  render_table(table, {}, out);
  while (!out.empty() && out.back() == '\n') {
    out.pop_back();
  }
  if (!out.empty()) {
    out.push_back('\n');
  }
  return out;
}

// ---------------------------------------------------------------------------
// Typed lookups
// ---------------------------------------------------------------------------

namespace {

Error missing_key(std::string_view key, std::string_view context) {
  Error error = Error::make("cova::toml::missing_key",
                            "missing key '" + std::string(key) + "' in " + std::string(context));
  return error;
}

Error wrong_type(std::string_view key, std::string_view context, std::string_view expected,
                 const Value& found) {
  Error error = Error::make("cova::toml::type", "expected " + std::string(expected) + " for '" +
                                                    std::string(key) + "' in " +
                                                    std::string(context) + ", found " +
                                                    found.type_name());
  if (found.location().line != 0) {
    error.with("line: " + std::to_string(found.location().line));
    error.with("column: " + std::to_string(found.location().column));
  }
  return error;
}

}  // namespace

Result<std::string> require_string(const Table& table, std::string_view key,
                                   std::string_view context) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return std::unexpected(missing_key(key, context));
  }
  if (!it->second.is_string() && it->second.kind() != Value::Kind::Datetime) {
    return std::unexpected(wrong_type(key, context, "a string", it->second));
  }
  return it->second.as_string();
}

Result<std::int64_t> require_integer(const Table& table, std::string_view key,
                                     std::string_view context) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return std::unexpected(missing_key(key, context));
  }
  if (!it->second.is_integer()) {
    return std::unexpected(wrong_type(key, context, "an integer", it->second));
  }
  return it->second.as_integer();
}

Result<bool> require_bool(const Table& table, std::string_view key, std::string_view context) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return std::unexpected(missing_key(key, context));
  }
  if (!it->second.is_bool()) {
    return std::unexpected(wrong_type(key, context, "a boolean", it->second));
  }
  return it->second.as_bool();
}

std::optional<std::string> optional_string(const Table& table, std::string_view key) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return std::nullopt;
  }
  if (!it->second.is_string() && it->second.kind() != Value::Kind::Datetime) {
    return std::nullopt;
  }
  return it->second.as_string();
}

}  // namespace cova::toml
