// Minimal dependency-free test harness (keeps the wasm test binary small and
// lets tests run identically under node and natively).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace khonsu::test {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

struct Register {
  Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace khonsu::test

#define KH_CONCAT2(a, b) a##b
#define KH_CONCAT(a, b) KH_CONCAT2(a, b)
#define TEST(name)                                                                     \
  static void KH_CONCAT(test_fn_, __LINE__)();                                         \
  static ::khonsu::test::Register KH_CONCAT(test_reg_, __LINE__)(name, &KH_CONCAT(test_fn_, __LINE__)); \
  static void KH_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                        \
  do {                                                                                     \
    if (!(cond)) {                                                                         \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
      ++::khonsu::test::failures();                                                        \
    }                                                                                      \
  } while (0)

#define REQUIRE(cond)                                                                      \
  do {                                                                                     \
    if (!(cond)) {                                                                         \
      std::printf("    FAIL %s:%d: %s (fatal)\n", __FILE__, __LINE__, #cond);              \
      ++::khonsu::test::failures();                                                        \
      return;                                                                              \
    }                                                                                      \
  } while (0)
