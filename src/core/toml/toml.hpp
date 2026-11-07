// Cova — TOML reader/writer for the manifest and lockfile formats (PRD §5.3).
//
// Implements the TOML subset Cova needs and that its own manifests use:
//   * comments, bare and quoted keys, dotted keys
//   * [table] and [[array-of-tables]]
//   * basic strings with escapes, literal strings, multi-line strings
//   * integers (decimal/hex/octal/binary, underscores), floats, booleans
//   * arrays (mixed nesting), inline tables
//   * datetimes are read as raw strings
// Line/column information is retained for every value so diagnostics can point
// at the offending manifest line (PRD §19.3).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova::toml {

class Value;

using Table = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

struct SourceLocation {
  std::uint32_t line = 0;   // 1-based
  std::uint32_t column = 0; // 1-based
};

class Value {
 public:
  enum class Kind { Invalid, Boolean, Integer, Float, String, Array, Table, Datetime };

  Value();
  Value(bool v);
  Value(std::int64_t v);
  Value(double v);
  Value(std::string v);
  Value(Array v);
  Value(Table v);

  static Value string_table();
  // Builds a Datetime value. Datetimes are retained as their raw source text
  // (see the header comment); this factory exists so the parser can preserve
  // the kind (additive change, core-dev).
  static Value datetime(std::string raw);

  Kind kind() const { return kind_; }
  bool valid() const { return kind_ != Kind::Invalid; }
  bool is_bool() const { return kind_ == Kind::Boolean; }
  bool is_integer() const { return kind_ == Kind::Integer; }
  bool is_float() const { return kind_ == Kind::Float; }
  bool is_number() const { return is_integer() || is_float(); }
  bool is_string() const { return kind_ == Kind::String; }
  bool is_array() const { return kind_ == Kind::Array; }
  bool is_table() const { return kind_ == Kind::Table; }

  bool as_bool(bool fallback = false) const;
  std::int64_t as_integer(std::int64_t fallback = 0) const;
  double as_float(double fallback = 0.0) const;
  std::string as_string(std::string_view fallback = {}) const;
  const Array* as_array() const;
  const Table* as_table() const;
  Array* as_array();
  Table* as_table();
  // String arrays with a single element are accepted as single values and
  // vice versa (used for `sources = "src/main.cpp"`).
  std::vector<std::string> as_string_array() const;

  const SourceLocation& location() const { return location_; }
  void set_location(SourceLocation loc) { location_ = loc; }

  std::string type_name() const;
  // Renders the value as TOML (used for `cova init` and lockfile output).
  std::string to_toml(int indent = 0) const;

 private:
  Kind kind_ = Kind::Invalid;
  bool bool_ = false;
  std::int64_t int_ = 0;
  double float_ = 0.0;
  std::string string_;
  Array array_;
  std::shared_ptr<Table> table_;  // shared so Values stay copyable
  SourceLocation location_;
};

// Parses a document. Errors carry line/column and a stable code
// "cova::toml::parse".
Result<Value> parse(std::string_view text, std::string_view source_name = "<toml>");
Result<Value> parse_file(const std::string& path);

// Serialises a table (top-level keys first, then sub-tables).
std::string write(const Value& value);
std::string write(const Table& table);

// Convenience typed lookup: returns an Error with a manifest-friendly message
// when the key is missing or has the wrong type.
Result<std::string> require_string(const Table& table, std::string_view key,
                                   std::string_view context);
Result<std::int64_t> require_integer(const Table& table, std::string_view key,
                                     std::string_view context);
Result<bool> require_bool(const Table& table, std::string_view key, std::string_view context);
std::optional<std::string> optional_string(const Table& table, std::string_view key);

}  // namespace cova::toml
