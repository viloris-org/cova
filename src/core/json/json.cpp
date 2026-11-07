// Cova — minimal JSON value model (implementation).
//
// Hand-written reader/writer: Cova must not depend on a third-party JSON
// library (PRD §16.3). Object keys are kept in a sorted map so that dumping is
// byte-for-byte deterministic (PRD §19.2, ARCHITECTURE §1.7).
#include "core/json/json.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "core/common.hpp"

namespace cova::json {

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

Value::Value() : storage_(nullptr) {}
Value::Value(std::nullptr_t) : storage_(nullptr) {}
Value::Value(bool v) : storage_(v) {}
Value::Value(int v) : storage_(static_cast<std::int64_t>(v)) {}
Value::Value(std::int64_t v) : storage_(v) {}
Value::Value(std::size_t v) : storage_(static_cast<std::int64_t>(v)) {}
Value::Value(double v) : storage_(v) {}
Value::Value(const char* v) : storage_(std::string(v == nullptr ? "" : v)) {}
Value::Value(std::string v) : storage_(std::move(v)) {}
Value::Value(Array v) : storage_(std::move(v)) {}
Value::Value(Object v) : storage_(std::move(v)) {}

Value Value::array() { return Value(Array{}); }
Value Value::object() { return Value(Object{}); }

bool Value::is_null() const { return std::holds_alternative<std::nullptr_t>(storage_); }
bool Value::is_bool() const { return std::holds_alternative<bool>(storage_); }
bool Value::is_number() const {
  return std::holds_alternative<std::int64_t>(storage_) || std::holds_alternative<double>(storage_);
}
bool Value::is_string() const { return std::holds_alternative<std::string>(storage_); }
bool Value::is_array() const { return std::holds_alternative<Array>(storage_); }
bool Value::is_object() const { return std::holds_alternative<Object>(storage_); }

bool Value::as_bool(bool fallback) const {
  const bool* value = std::get_if<bool>(&storage_);
  return value == nullptr ? fallback : *value;
}

std::int64_t Value::as_int(std::int64_t fallback) const {
  if (const std::int64_t* value = std::get_if<std::int64_t>(&storage_); value != nullptr) {
    return *value;
  }
  if (const double* value = std::get_if<double>(&storage_); value != nullptr) {
    return static_cast<std::int64_t>(*value);
  }
  if (const bool* value = std::get_if<bool>(&storage_); value != nullptr) {
    return *value ? 1 : 0;
  }
  return fallback;
}

std::string Value::as_string(std::string_view fallback) const {
  const std::string* value = std::get_if<std::string>(&storage_);
  return value == nullptr ? std::string(fallback) : *value;
}

const Array* Value::as_array() const { return std::get_if<Array>(&storage_); }
const Object* Value::as_object() const { return std::get_if<Object>(&storage_); }
Array* Value::as_array() { return std::get_if<Array>(&storage_); }
Object* Value::as_object() { return std::get_if<Object>(&storage_); }

Value& Value::set(std::string key, Value v) {
  if (Object* object = std::get_if<Object>(&storage_); object != nullptr) {
    object->insert_or_assign(std::move(key), std::move(v));
  }
  return *this;
}

const Value* Value::get(std::string_view key) const {
  const Object* object = std::get_if<Object>(&storage_);
  if (object == nullptr) {
    return nullptr;
  }
  const auto it = object->find(key);
  return it == object->end() ? nullptr : &it->second;
}

Value& Value::push(Value v) {
  if (Array* array = std::get_if<Array>(&storage_); array != nullptr) {
    array->push_back(std::move(v));
  }
  return *this;
}

bool Value::operator==(const Value& other) const {
  // std::variant's operator== compares the alternative index first; an int and
  // a double are therefore never equal, which keeps round-trip checks honest.
  return storage_ == other.storage_;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

namespace {

bool is_valid_utf8_continuation(unsigned char c) { return (c & 0xC0u) == 0x80u; }

void dump_string(std::string& out, std::string_view s) {
  out.push_back('"');
  for (std::size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
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
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else if (c < 0x80) {
          out.push_back(static_cast<char>(c));
        } else {
          // Pass valid UTF-8 through unchanged; replace invalid bytes so the
          // output is always consumable JSON.
          std::size_t length = 0;
          if ((c & 0xE0u) == 0xC0u) {
            length = 1;
          } else if ((c & 0xF0u) == 0xE0u) {
            length = 2;
          } else if ((c & 0xF8u) == 0xF0u) {
            length = 3;
          }
          bool valid = length != 0;
          if (valid) {
            for (std::size_t k = 1; k <= length; ++k) {
              if (i + k >= s.size() ||
                  !is_valid_utf8_continuation(static_cast<unsigned char>(s[i + k]))) {
                valid = false;
                break;
              }
            }
          }
          if (valid) {
            out.append(s.substr(i, length + 1));
            i += length;
          } else {
            out += "\\ufffd";
          }
        }
    }
  }
  out.push_back('"');
}

std::string format_double(double value) {
  if (!std::isfinite(value)) {
    return "null";  // JSON has no representation for NaN/Infinity
  }
  if (value == static_cast<double>(static_cast<std::int64_t>(value)) &&
      std::abs(value) < 1e15) {
    // Integral values print without a decimal tail, matching common writers.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    return std::string(buffer);
  }
  char buffer[40];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec == std::errc()) {
    return std::string(buffer, result.ptr);
  }
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return std::string(buffer);
}

void dump_into(const Value& value, std::string& out, int indent, int depth) {
  const bool pretty = indent > 0;
  const std::string pad(pretty ? static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth)
                               : 0,
                        ' ');
  const std::string pad_inner(
      pretty ? static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth + 1) : 0, ' ');

  if (const auto* null_value = std::get_if<std::nullptr_t>(&value.storage()); null_value) {
    out += "null";
  } else if (const auto* boolean = std::get_if<bool>(&value.storage()); boolean) {
    out += *boolean ? "true" : "false";
  } else if (const auto* number = std::get_if<std::int64_t>(&value.storage()); number) {
    out += std::to_string(*number);
  } else if (const auto* real = std::get_if<double>(&value.storage()); real) {
    out += format_double(*real);
  } else if (const auto* text = std::get_if<std::string>(&value.storage()); text) {
    dump_string(out, *text);
  } else if (const auto* array = std::get_if<Array>(&value.storage()); array) {
    if (array->empty()) {
      out += "[]";
      return;
    }
    out.push_back('[');
    for (std::size_t i = 0; i < array->size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      if (pretty) {
        out.push_back('\n');
        out += pad_inner;
      }
      dump_into((*array)[i], out, indent, depth + 1);
    }
    if (pretty) {
      out.push_back('\n');
      out += pad;
    }
    out.push_back(']');
  } else if (const auto* object = std::get_if<Object>(&value.storage()); object) {
    if (object->empty()) {
      out += "{}";
      return;
    }
    out.push_back('{');
    bool first = true;
    for (const auto& [key, entry] : *object) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      if (pretty) {
        out.push_back('\n');
        out += pad_inner;
      }
      dump_string(out, key);
      out.push_back(':');
      if (pretty) {
        out.push_back(' ');
      }
      dump_into(entry, out, indent, depth + 1);
    }
    if (pretty) {
      out.push_back('\n');
      out += pad;
    }
    out.push_back('}');
  }
}

// --- parser ---------------------------------------------------------------

class Parser {
 public:
  Parser(std::string_view text, std::string_view source) : text_(text), source_(source) {}

  Result<Value> parse_document() {
    skip_whitespace();
    auto value = parse_value(0);
    if (!value) {
      return value;
    }
    skip_whitespace();
    if (pos_ != text_.size()) {
      return error("trailing characters after JSON document");
    }
    return value;
  }

 private:
  static constexpr int kMaxDepth = 256;

  Result<Value> error(std::string message) {
    Error err = Error::make("cova::json::parse", std::move(message));
    err.with("source: " + std::string(source_));
    err.with("offset: " + std::to_string(pos_ + 1));
    return std::unexpected(std::move(err));
  }

  void skip_whitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  Result<Value> parse_value(int depth) {
    if (depth > kMaxDepth) {
      return error("JSON nesting is too deep");
    }
    if (pos_ >= text_.size()) {
      return error("unexpected end of input");
    }
    switch (text_[pos_]) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"':
        return parse_string_value();
      case 't':
        return parse_literal("true", Value(true));
      case 'f':
        return parse_literal("false", Value(false));
      case 'n':
        return parse_literal("null", Value(nullptr));
      default:
        return parse_number();
    }
  }

  Result<Value> parse_literal(std::string_view word, Value value) {
    if (text_.substr(pos_, word.size()) != word) {
      return error("invalid JSON literal");
    }
    pos_ += word.size();
    return value;
  }

  Result<Value> parse_object(int depth) {
    ++pos_;  // '{'
    Object object;
    skip_whitespace();
    if (consume('}')) {
      return Value(std::move(object));
    }
    for (;;) {
      skip_whitespace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return error("expected a quoted object key");
      }
      auto key = parse_string();
      if (!key) {
        return std::unexpected(std::move(key.error()));
      }
      skip_whitespace();
      if (!consume(':')) {
        return error("expected ':' after object key");
      }
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value) {
        return value;
      }
      object.insert_or_assign(std::move(*key), std::move(*value));
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return Value(std::move(object));
      }
      return error("expected ',' or '}' in object");
    }
  }

  Result<Value> parse_array(int depth) {
    ++pos_;  // '['
    Array array;
    skip_whitespace();
    if (consume(']')) {
      return Value(std::move(array));
    }
    for (;;) {
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value) {
        return value;
      }
      array.push_back(std::move(*value));
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return Value(std::move(array));
      }
      return error("expected ',' or ']' in array");
    }
  }

  Result<Value> parse_string_value() {
    auto text = parse_string();
    if (!text) {
      return std::unexpected(std::move(text.error()));
    }
    return Value(std::move(*text));
  }

  static void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point < 0x10000) {
      out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
  }

  Result<std::string> parse_string() {
    ++pos_;  // opening quote
    std::string out;
    for (;;) {
      if (pos_ >= text_.size()) {
        return std::unexpected(error("unterminated JSON string").error());
      }
      const char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        return out;
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) {
          return std::unexpected(error("unterminated escape sequence").error());
        }
        const char escape = text_[pos_++];
        switch (escape) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            if (pos_ + 4 > text_.size()) {
              return std::unexpected(error("truncated \\u escape").error());
            }
            std::uint32_t code_point = 0;
            for (int i = 0; i < 4; ++i) {
              const char digit = text_[pos_ + static_cast<std::size_t>(i)];
              code_point <<= 4;
              if (digit >= '0' && digit <= '9') {
                code_point |= static_cast<std::uint32_t>(digit - '0');
              } else if (digit >= 'a' && digit <= 'f') {
                code_point |= static_cast<std::uint32_t>(digit - 'a' + 10);
              } else if (digit >= 'A' && digit <= 'F') {
                code_point |= static_cast<std::uint32_t>(digit - 'A' + 10);
              } else {
                return std::unexpected(error("invalid \\u escape").error());
              }
            }
            pos_ += 4;
            if (code_point >= 0xD800 && code_point <= 0xDBFF && pos_ + 6 <= text_.size() &&
                text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
              std::uint32_t low = 0;
              bool valid = true;
              for (int i = 0; i < 4; ++i) {
                const char digit = text_[pos_ + 2 + static_cast<std::size_t>(i)];
                low <<= 4;
                if (digit >= '0' && digit <= '9') {
                  low |= static_cast<std::uint32_t>(digit - '0');
                } else if (digit >= 'a' && digit <= 'f') {
                  low |= static_cast<std::uint32_t>(digit - 'a' + 10);
                } else if (digit >= 'A' && digit <= 'F') {
                  low |= static_cast<std::uint32_t>(digit - 'A' + 10);
                } else {
                  valid = false;
                  break;
                }
              }
              if (valid && low >= 0xDC00 && low <= 0xDFFF) {
                code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                pos_ += 6;
              }
            }
            append_utf8(out, code_point);
            break;
          }
          default:
            return std::unexpected(error("invalid escape sequence").error());
        }
        continue;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return std::unexpected(error("control character in JSON string").error());
      }
      out.push_back(c);
      ++pos_;
    }
  }

  Result<Value> parse_number() {
    const std::size_t begin = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
      if (text_[pos_] == '+') {
        return error("invalid number: leading '+' is not allowed in JSON");
      }
      ++pos_;
    }
    if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
      return error("invalid JSON value");
    }
    if (text_[pos_] == '0') {
      ++pos_;
    } else {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    bool is_float = false;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      is_float = true;
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        return error("invalid number: expected digits after '.'");
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      is_float = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
        return error("invalid number: expected digits in exponent");
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    const std::string_view token = text_.substr(begin, pos_ - begin);
    if (is_float) {
      double value = 0.0;
      const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
      if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
        return error("invalid floating point number");
      }
      return Value(value);
    }
    std::int64_t value = 0;
    const auto result =
        std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
      return error("invalid integer");
    }
    return Value(value);
  }

  std::string_view text_;
  std::string_view source_;
  std::size_t pos_ = 0;
};

}  // namespace

std::string escape(std::string_view s) {
  std::string out;
  dump_string(out, s);
  // dump_string adds the surrounding quotes; `escape` is documented to return
  // the escaped body, so strip them.
  if (out.size() >= 2) {
    return out.substr(1, out.size() - 2);
  }
  return out;
}

std::string Value::dump(int indent) const {
  std::string out;
  dump_into(*this, out, indent, 0);
  return out;
}

Result<Value> Value::parse(std::string_view text) { return Parser(text, "<json>").parse_document(); }

}  // namespace cova::json
