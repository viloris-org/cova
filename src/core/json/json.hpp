// Cova — minimal JSON value model for structured output (PRD §19.3, §4 metadata).
//
// `cova metadata --format json`, `cova graph --format json`, `cova explain
// --format json` and machine-readable fmt output all use this writer.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/common.hpp"

namespace cova::json {

class Value;

using Object = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

class Value {
 public:
  using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                               Array, Object>;

  Value();  // null
  Value(std::nullptr_t);
  Value(bool v);
  Value(int v);
  Value(std::int64_t v);
  Value(std::size_t v);
  Value(double v);
  Value(const char* v);
  Value(std::string v);
  Value(Array v);
  Value(Object v);

  static Value array();
  static Value object();

  const Storage& storage() const { return storage_; }
  bool is_null() const;
  bool is_bool() const;
  bool is_number() const;
  bool is_string() const;
  bool is_array() const;
  bool is_object() const;

  // Typed accessors; return the fallback when the type does not match.
  bool as_bool(bool fallback = false) const;
  std::int64_t as_int(std::int64_t fallback = 0) const;
  std::string as_string(std::string_view fallback = {}) const;
  const Array* as_array() const;
  const Object* as_object() const;
  Array* as_array();
  Object* as_object();

  // Object mutation helpers (no-ops for non-objects).
  Value& set(std::string key, Value v);
  const Value* get(std::string_view key) const;
  // Array append (no-op for non-arrays).
  Value& push(Value v);

  // Serialisation. `indent` of 0 produces compact single-line output.
  std::string dump(int indent = 2) const;

  static Result<Value> parse(std::string_view text);

  bool operator==(const Value&) const;

 private:
  Storage storage_;
};

// Convenience string escaping used by the writer and by hand-built JSON.
std::string escape(std::string_view s);

inline Value make_object() { return Value::object(); }
inline Value make_array() { return Value::array(); }

}  // namespace cova::json
