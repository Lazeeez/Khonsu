#include "check.hpp"

#include <chrono>
#include <cstring>

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const auto& c : khonsu::test::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = khonsu::test::failures();
    const auto t0 = std::chrono::steady_clock::now();
    c.fn();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s %-60s %8.2f ms\n", khonsu::test::failures() == before ? "  ok " : "  ERR", c.name, ms);
    ++ran;
  }
  std::printf("\n%d tests, %d failed checks\n", ran, khonsu::test::failures());
  return khonsu::test::failures() == 0 ? 0 : 1;
}
