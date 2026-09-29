// Khonsu — JSON value model for recordings.
//
// Why not a general-purpose JSON library: request matching hashes a *canonical*
// serialization of each request body, so canonicalization must be exactly
// reproducible across machines, compilers and library versions. Numbers are
// therefore kept as their original lexemes (no double round-trip: 1e-5 stays
// "1e-5", 0.10 stays "0.10"), object keys are sorted bytewise, and strings are
// re-escaped with one fixed policy.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace khonsu::jsonv {

enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
public:
  using Array = std::vector<Value>;
  using Object = std::vector<std::pair<std::string, Value>>;  // insertion order kept

  Value() = default;  // null
  static Value boolean(bool b);
  static Value number(std::string lexeme);  // caller guarantees a valid JSON number
  static Value string(std::string s);
  static Value array(Array a = {});
  static Value object(Object o = {});

  [[nodiscard]] Kind kind() const { return kind_; }
  [[nodiscard]] bool is_null() const { return kind_ == Kind::Null; }
  [[nodiscard]] bool as_bool() const { return b_; }
  [[nodiscard]] const std::string& as_string() const { return s_; }  // String, or Number lexeme
  [[nodiscard]] const Array& as_array() const { return a_; }
  [[nodiscard]] const Object& as_object() const { return o_; }
  Array& as_array() { return a_; }
  Object& as_object() { return o_; }

  // Object member lookup (first match); nullptr when absent or not an object.
  [[nodiscard]] const Value* get(std::string_view key) const;
  // Array element; nullptr when out of range or not an array.
  [[nodiscard]] const Value* at(std::size_t i) const;
  // Convenience: string member or empty.
  [[nodiscard]] std::string_view str(std::string_view key) const;
  void set(std::string key, Value v);  // replace or append (objects only)
  void erase(std::string_view key);

  friend bool operator==(const Value& a, const Value& b);

private:
  Kind kind_ = Kind::Null;
  bool b_ = false;
  std::string s_;
  Array a_;
  Object o_;
};

struct ParseError {
  std::size_t offset = 0;
  std::string message;
};

// Strict RFC 8259 parser (depth-limited). Returns nullopt and fills `err` on failure.
[[nodiscard]] std::optional<Value> parse(std::string_view text, ParseError* err = nullptr);

// Canonical form: no whitespace, object keys sorted bytewise (duplicates keep the
// last value, as most parsers do), strings escaped minimally with lowercase \u00xx
// for control characters, numbers as original lexemes.
[[nodiscard]] std::string canonical(const Value& v);

// Pretty form for humans (2-space indent, keys in original order).
[[nodiscard]] std::string pretty(const Value& v, int indent = 2);

// Path of the first difference between two values ("" when equal), e.g.
// "messages[3].content". Arrays compare element-wise, objects by sorted key.
[[nodiscard]] std::string first_difference(const Value& a, const Value& b);

}  // namespace khonsu::jsonv
