// Khonsu — micro-benchmarks shared by the native CLI and the in-browser /bench page.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace khonsu::bench {

struct Config {
  std::vector<std::uint32_t> state_pages = {256, 1024, 4096, 16384};  // 1 MiB .. 64 MiB of VFS
  std::uint32_t files = 64;
  std::uint32_t reps = 15;
  std::uint32_t log_frames = 200'000;
  std::uint32_t crc_mib = 32;
  bool include_deep_copy = true;
};

// Runs every suite and returns a JSON document. `progress` (optional) receives
// human-readable status lines as suites complete.
std::string run_all(const Config& cfg, const std::function<void(const std::string&)>& progress = {});

}  // namespace khonsu::bench
