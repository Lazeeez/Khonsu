#include "khonsu/json.hpp"

#include <cmath>
#include <cstdio>

namespace khonsu {

void Json::comma() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (!first_.empty()) {
    if (!first_.back()) out_ += ',';
    first_.back() = false;
  }
}

Json& Json::begin_object() {
  comma();
  out_ += '{';
  first_.push_back(true);
  return *this;
}
Json& Json::end_object() {
  out_ += '}';
  first_.pop_back();
  return *this;
}
Json& Json::begin_array() {
  comma();
  out_ += '[';
  first_.push_back(true);
  return *this;
}
Json& Json::end_array() {
  out_ += ']';
  first_.pop_back();
  return *this;
}

Json& Json::key(std::string_view k) {
  comma();
  write_string(k);
  out_ += ':';
  after_key_ = true;
  return *this;
}

Json& Json::value(std::string_view s) {
  comma();
  write_string(s);
  return *this;
}

void Json::write_string(std::string_view s) {
  out_ += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out_ += "\\\""; break;
      case '\\': out_ += "\\\\"; break;
      case '\n': out_ += "\\n"; break;
      case '\r': out_ += "\\r"; break;
      case '\t': out_ += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out_ += buf;
        } else {
          out_ += ch;
        }
    }
  }
  out_ += '"';
}

Json& Json::value(bool b) {
  comma();
  out_ += b ? "true" : "false";
  return *this;
}

Json& Json::value(double d) {
  comma();
  if (!std::isfinite(d)) {
    out_ += "null";
    return *this;
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6g", d);
  out_ += buf;
  return *this;
}

Json& Json::value(std::int64_t v) {
  comma();
  out_ += std::to_string(v);
  return *this;
}

Json& Json::value(std::uint64_t v) {
  comma();
  out_ += std::to_string(v);
  return *this;
}

Json& Json::null() {
  comma();
  out_ += "null";
  return *this;
}

Json& Json::raw(std::string_view already_json) {
  comma();
  out_ += already_json;
  return *this;
}

}  // namespace khonsu
