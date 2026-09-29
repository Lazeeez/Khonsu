#include "khonsu/jsonv.hpp"

#include <algorithm>
#include <cstdio>

namespace khonsu::jsonv {

// ---------------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------------

Value Value::boolean(bool b) {
  Value v;
  v.kind_ = Kind::Bool;
  v.b_ = b;
  return v;
}
Value Value::number(std::string lexeme) {
  Value v;
  v.kind_ = Kind::Number;
  v.s_ = std::move(lexeme);
  return v;
}
Value Value::string(std::string s) {
  Value v;
  v.kind_ = Kind::String;
  v.s_ = std::move(s);
  return v;
}
Value Value::array(Array a) {
  Value v;
  v.kind_ = Kind::Array;
  v.a_ = std::move(a);
  return v;
}
Value Value::object(Object o) {
  Value v;
  v.kind_ = Kind::Object;
  v.o_ = std::move(o);
  return v;
}

const Value* Value::get(std::string_view key) const {
  if (kind_ != Kind::Object) return nullptr;
  for (const auto& [k, v] : o_)
    if (k == key) return &v;
  return nullptr;
}

const Value* Value::at(std::size_t i) const {
  if (kind_ != Kind::Array || i >= a_.size()) return nullptr;
  return &a_[i];
}

std::string_view Value::str(std::string_view key) const {
  const Value* v = get(key);
  return v && v->kind_ == Kind::String ? std::string_view(v->s_) : std::string_view();
}

void Value::set(std::string key, Value v) {
  if (kind_ != Kind::Object) return;
  for (auto& [k, old] : o_)
    if (k == key) {
      old = std::move(v);
      return;
    }
  o_.emplace_back(std::move(key), std::move(v));
}

void Value::erase(std::string_view key) {
  if (kind_ != Kind::Object) return;
  o_.erase(std::remove_if(o_.begin(), o_.end(), [&](const auto& kv) { return kv.first == key; }), o_.end());
}

bool operator==(const Value& a, const Value& b) {
  if (a.kind_ != b.kind_) return false;
  switch (a.kind_) {
    case Kind::Null: return true;
    case Kind::Bool: return a.b_ == b.b_;
    case Kind::Number:
    case Kind::String: return a.s_ == b.s_;
    case Kind::Array: return a.a_ == b.a_;
    case Kind::Object: return canonical(a) == canonical(b);
  }
  return false;
}

// ---------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------

namespace {

constexpr int kMaxDepth = 256;

class Parser {
public:
  explicit Parser(std::string_view t) : t_(t) {}

  std::optional<Value> run(ParseError* err) {
    ws();
    auto v = value(0);
    if (v) {
      ws();
      if (p_ != t_.size()) v = fail("trailing characters");
    }
    if (!v && err) *err = err_;
    return v;
  }

private:
  std::optional<Value> fail(const char* msg) {
    if (err_.message.empty()) err_ = {p_, msg};
    return std::nullopt;
  }
  void ws() {
    while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
  }
  bool lit(std::string_view s) {
    if (t_.substr(p_, s.size()) != s) return false;
    p_ += s.size();
    return true;
  }

  std::optional<Value> value(int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    if (p_ >= t_.size()) return fail("unexpected end of input");
    switch (t_[p_]) {
      case '{': return object(depth);
      case '[': return array(depth);
      case '"': {
        auto s = string();
        if (!s) return std::nullopt;
        return Value::string(std::move(*s));
      }
      case 't': return lit("true") ? std::optional(Value::boolean(true)) : fail("invalid literal");
      case 'f': return lit("false") ? std::optional(Value::boolean(false)) : fail("invalid literal");
      case 'n': return lit("null") ? std::optional(Value()) : fail("invalid literal");
      default: return number();
    }
  }

  std::optional<Value> number() {
    const std::size_t start = p_;
    auto digits = [&] {
      const std::size_t s = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      return p_ > s;
    };
    if (p_ < t_.size() && t_[p_] == '-') ++p_;
    if (p_ < t_.size() && t_[p_] == '0') {
      ++p_;
    } else if (!digits()) {
      return fail("invalid value");
    }
    if (p_ < t_.size() && t_[p_] == '.') {
      ++p_;
      if (!digits()) return fail("invalid number fraction");
    }
    if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
      ++p_;
      if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
      if (!digits()) return fail("invalid number exponent");
    }
    return Value::number(std::string(t_.substr(start, p_ - start)));
  }

  static void put_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(std::uint32_t& out) {
    if (t_.size() - p_ < 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[p_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
      else return false;
    }
    return true;
  }

  std::optional<std::string> string() {
    ++p_;  // opening quote
    std::string out;
    while (true) {
      if (p_ >= t_.size()) {
        fail("unterminated string");
        return std::nullopt;
      }
      const char c = t_[p_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) {
        fail("control character in string");
        return std::nullopt;
      }
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p_ >= t_.size()) {
        fail("unterminated escape");
        return std::nullopt;
      }
      const char e = t_[p_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          std::uint32_t cp;
          if (!hex4(cp)) {
            fail("invalid \\u escape");
            return std::nullopt;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
            std::uint32_t lo;
            if (t_.substr(p_, 2) == "\\u" && (p_ += 2, hex4(lo)) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              cp = 0xFFFD;  // lone surrogate: replacement character, deterministically
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          put_utf8(out, cp);
          break;
        }
        default:
          fail("invalid escape");
          return std::nullopt;
      }
    }
  }

  std::optional<Value> array(int depth) {
    ++p_;
    Value::Array a;
    ws();
    if (p_ < t_.size() && t_[p_] == ']') {
      ++p_;
      return Value::array(std::move(a));
    }
    while (true) {
      ws();
      auto v = value(depth + 1);
      if (!v) return std::nullopt;
      a.push_back(std::move(*v));
      ws();
      if (p_ < t_.size() && t_[p_] == ',') {
        ++p_;
        continue;
      }
      if (p_ < t_.size() && t_[p_] == ']') {
        ++p_;
        return Value::array(std::move(a));
      }
      return fail("expected ',' or ']'");
    }
  }

  std::optional<Value> object(int depth) {
    ++p_;
    Value::Object o;
    ws();
    if (p_ < t_.size() && t_[p_] == '}') {
      ++p_;
      return Value::object(std::move(o));
    }
    while (true) {
      ws();
      if (p_ >= t_.size() || t_[p_] != '"') return fail("expected object key");
      auto k = string();
      if (!k) return std::nullopt;
      ws();
      if (p_ >= t_.size() || t_[p_] != ':') return fail("expected ':'");
      ++p_;
      ws();
      auto v = value(depth + 1);
      if (!v) return std::nullopt;
      o.emplace_back(std::move(*k), std::move(*v));
      ws();
      if (p_ < t_.size() && t_[p_] == ',') {
        ++p_;
        continue;
      }
      if (p_ < t_.size() && t_[p_] == '}') {
        ++p_;
        return Value::object(std::move(o));
      }
      return fail("expected ',' or '}'");
    }
  }

  std::string_view t_;
  std::size_t p_ = 0;
  ParseError err_;
};

void escape_into(std::string& out, std::string_view s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

// Sorted (key, value*) view of an object; duplicate keys keep the last value.
std::vector<std::pair<std::string_view, const Value*>> sorted_members(const Value::Object& o) {
  std::vector<std::pair<std::string_view, const Value*>> m;
  m.reserve(o.size());
  for (const auto& [k, v] : o) m.emplace_back(k, &v);
  std::stable_sort(m.begin(), m.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::pair<std::string_view, const Value*>> out;
  out.reserve(m.size());
  for (std::size_t i = 0; i < m.size(); ++i)
    if (i + 1 == m.size() || m[i + 1].first != m[i].first) out.push_back(m[i]);
  return out;
}

void canonical_into(std::string& out, const Value& v) {
  switch (v.kind()) {
    case Kind::Null: out += "null"; break;
    case Kind::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Kind::Number: out += v.as_string(); break;
    case Kind::String: escape_into(out, v.as_string()); break;
    case Kind::Array: {
      out += '[';
      bool first = true;
      for (const auto& e : v.as_array()) {
        if (!first) out += ',';
        first = false;
        canonical_into(out, e);
      }
      out += ']';
      break;
    }
    case Kind::Object: {
      out += '{';
      bool first = true;
      for (const auto& [k, e] : sorted_members(v.as_object())) {
        if (!first) out += ',';
        first = false;
        escape_into(out, k);
        out += ':';
        canonical_into(out, *e);
      }
      out += '}';
      break;
    }
  }
}

void pretty_into(std::string& out, const Value& v, int indent, int level) {
  const std::string pad(static_cast<std::size_t>(indent * (level + 1)), ' ');
  const std::string close(static_cast<std::size_t>(indent * level), ' ');
  switch (v.kind()) {
    case Kind::Array:
      if (v.as_array().empty()) {
        out += "[]";
        return;
      }
      out += "[\n";
      for (std::size_t i = 0; i < v.as_array().size(); ++i) {
        out += pad;
        pretty_into(out, v.as_array()[i], indent, level + 1);
        out += i + 1 < v.as_array().size() ? ",\n" : "\n";
      }
      out += close + "]";
      return;
    case Kind::Object:
      if (v.as_object().empty()) {
        out += "{}";
        return;
      }
      out += "{\n";
      for (std::size_t i = 0; i < v.as_object().size(); ++i) {
        out += pad;
        escape_into(out, v.as_object()[i].first);
        out += ": ";
        pretty_into(out, v.as_object()[i].second, indent, level + 1);
        out += i + 1 < v.as_object().size() ? ",\n" : "\n";
      }
      out += close + "}";
      return;
    default:
      canonical_into(out, v);
  }
}

std::string diff_rec(const Value& a, const Value& b, const std::string& path) {
  if (a.kind() != b.kind()) return path.empty() ? "$" : path;
  switch (a.kind()) {
    case Kind::Array: {
      const auto& x = a.as_array();
      const auto& y = b.as_array();
      for (std::size_t i = 0; i < std::min(x.size(), y.size()); ++i) {
        auto d = diff_rec(x[i], y[i], path + "[" + std::to_string(i) + "]");
        if (!d.empty()) return d;
      }
      if (x.size() != y.size()) return path + "[" + std::to_string(std::min(x.size(), y.size())) + "]";
      return {};
    }
    case Kind::Object: {
      const auto x = sorted_members(a.as_object());
      const auto y = sorted_members(b.as_object());
      std::size_t i = 0, j = 0;
      while (i < x.size() || j < y.size()) {
        if (j == y.size() || (i < x.size() && x[i].first < y[j].first))
          return path + (path.empty() ? "" : ".") + std::string(x[i].first);
        if (i == x.size() || y[j].first < x[i].first)
          return path + (path.empty() ? "" : ".") + std::string(y[j].first);
        auto d = diff_rec(*x[i].second, *y[j].second, path + (path.empty() ? "" : ".") + std::string(x[i].first));
        if (!d.empty()) return d;
        ++i;
        ++j;
      }
      return {};
    }
    default:
      return a == b ? std::string() : (path.empty() ? "$" : path);
  }
}

}  // namespace

std::optional<Value> parse(std::string_view text, ParseError* err) { return Parser(text).run(err); }

std::string canonical(const Value& v) {
  std::string out;
  canonical_into(out, v);
  return out;
}

std::string pretty(const Value& v, int indent) {
  std::string out;
  pretty_into(out, v, indent, 0);
  return out;
}

std::string first_difference(const Value& a, const Value& b) { return diff_rec(a, b, ""); }

}  // namespace khonsu::jsonv
