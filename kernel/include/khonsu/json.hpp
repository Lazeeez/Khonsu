// Tiny streaming JSON writer for the cold-path UI bridge.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace khonsu {

class Json {
public:
  Json& begin_object();
  Json& end_object();
  Json& begin_array();
  Json& end_array();
  Json& key(std::string_view k);
  Json& value(std::string_view s);
  Json& value(const char* s) { return value(std::string_view(s)); }
  Json& value(const std::string& s) { return value(std::string_view(s)); }
  Json& value(bool b);
  Json& value(double d);
  Json& value(std::int64_t v);
  Json& value(std::uint64_t v);
  template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, std::int64_t> &&
             !std::is_same_v<T, std::uint64_t>)
  Json& value(T v) {
    if constexpr (std::is_signed_v<T>) return value(static_cast<std::int64_t>(v));
    else return value(static_cast<std::uint64_t>(v));
  }
  Json& null();
  Json& raw(std::string_view already_json);

  template <class T>
  Json& field(std::string_view k, const T& v) {
    key(k);
    return value(v);
  }

  [[nodiscard]] const std::string& str() const { return out_; }
  [[nodiscard]] std::string take() { return std::move(out_); }

private:
  void comma();
  void write_string(std::string_view s);
  std::string out_;
  std::vector<bool> first_;  // per nesting level: no element written yet
  bool after_key_ = false;
};

}  // namespace khonsu
