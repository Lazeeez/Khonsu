// Native / node entry point for the benchmark suite. Prints JSON to stdout,
// progress to stderr.  Usage: khonsu_bench [--quick]
#include "khonsu/bench.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  khonsu::bench::Config cfg;
  if (argc > 1 && std::strcmp(argv[1], "--quick") == 0) {
    cfg.state_pages = {256, 1024, 4096};
    cfg.reps = 7;
    cfg.log_frames = 50'000;
    cfg.crc_mib = 8;
  } else if (argc > 1 && std::strcmp(argv[1], "--large") == 0) {
    cfg.state_pages = {256, 1024, 4096, 16384, 65536};
  }
  const std::string json =
      khonsu::bench::run_all(cfg, [](const std::string& s) { std::fprintf(stderr, "[bench] %s\n", s.c_str()); });
  std::printf("%s\n", json.c_str());
  return 0;
}
