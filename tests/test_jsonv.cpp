#include "check.hpp"
#include "khonsu/jsonv.hpp"

using namespace khonsu;

TEST("jsonv: canonical form sorts keys, strips whitespace, keeps number lexemes") {
  const auto a = jsonv::parse(R"({ "b": 1e-5, "a": [ 0.10, -0, true, null ], "c": {"y": "\u00e9", "x": "\n"} })");
  const auto b = jsonv::parse(R"({"c":{"x":"\n","y":"é"},"a":[0.10,-0,true,null],"b":1e-5})");
  REQUIRE(a && b);
  CHECK(jsonv::canonical(*a) == jsonv::canonical(*b));
  CHECK(jsonv::canonical(*a) == R"({"a":[0.10,-0,true,null],"b":1e-5,"c":{"x":"\n","y":"é"}})");
}

TEST("jsonv: duplicate keys keep the last value in canonical form") {
  const auto v = jsonv::parse(R"({"k":1,"k":2})");
  REQUIRE(v);
  CHECK(jsonv::canonical(*v) == R"({"k":2})");
  CHECK(v->get("k")->as_string() == "2");  // readers agree with the canonical form
}

TEST("jsonv: a lone high surrogate does not swallow the escape after it") {
  const auto v = jsonv::parse(R"(["\ud800A","\ud800\uZZZZ"])");
  REQUIRE(v.has_value() == false);  // bad hex in the second escape is an error, not skipped
  const auto w = jsonv::parse(R"(["\ud800A"])");
  REQUIRE(w);
  CHECK(w->at(0)->as_string() == "\xEF\xBF\xBD" "A");
}

TEST("jsonv: escapes, surrogate pairs and control characters") {
  const auto v = jsonv::parse(R"(["\ud83c\udf19","\u0001","a\/b","\ud800x"])");
  REQUIRE(v);
  CHECK(v->at(0)->as_string() == "\xF0\x9F\x8C\x99");   // 🌙
  CHECK(v->at(2)->as_string() == "a/b");
  CHECK(v->at(3)->as_string() == "\xEF\xBF\xBDx");      // lone surrogate → U+FFFD
  CHECK(jsonv::canonical(*v) == "[\"\xF0\x9F\x8C\x99\",\"\\u0001\",\"a/b\",\"\xEF\xBF\xBDx\"]");
}

TEST("jsonv: rejects malformed input with an offset") {
  jsonv::ParseError e;
  CHECK(!jsonv::parse(R"({"a":01})", &e));
  CHECK(!jsonv::parse(R"({"a":1,})", &e));
  CHECK(!jsonv::parse("[1,2", &e));
  CHECK(!jsonv::parse("\"tab\there\"", &e));
  CHECK(!jsonv::parse("{} x", &e));
  CHECK(e.offset == 3);
  std::string deep(300, '[');
  CHECK(!jsonv::parse(deep, &e));
}

TEST("jsonv: first_difference names the path of the change") {
  const auto a = jsonv::parse(R"({"model":"m","messages":[{"role":"user","content":"hi"},{"role":"user","content":"x"}]})");
  const auto b = jsonv::parse(R"({"messages":[{"content":"hi","role":"user"},{"role":"user","content":"y"}],"model":"m"})");
  REQUIRE(a && b);
  CHECK(jsonv::first_difference(*a, *a).empty());
  CHECK(jsonv::first_difference(*a, *b) == "messages[1].content");
  const auto c = jsonv::parse(R"({"model":"m","messages":[{"role":"user","content":"hi"}]})");
  CHECK(jsonv::first_difference(*a, *c) == "messages[1]");
}
