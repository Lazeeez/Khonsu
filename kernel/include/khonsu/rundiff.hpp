// Khonsu — compare two recorded runs call by call.
//
// "Why did today's run differ from yesterday's?" The calls of both runs are
// aligned by their match keys (longest common subsequence), so an inserted or
// removed call doesn't shift every later comparison. Each aligned pair is then
// classified, and the first divergence is reported with the JSON field that
// differs, alongside how the workspaces differ.
#pragma once

#include "khonsu/recording.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace khonsu::rec {

enum class CallDiffKind : std::uint8_t {
  Same,             // same request, same output
  SameRequest,      // same request, different output (the model sampled differently)
  Changed,          // the request itself differs (prompt, arguments, tool results fed back…)
  OnlyA,            // call exists only in run A
  OnlyB,            // call exists only in run B
};

struct CallDiff {
  CallDiffKind kind = CallDiffKind::Same;
  const Exchange* a = nullptr;
  const Exchange* b = nullptr;
  std::string field;               // Changed: first differing request field
  std::string a_value, b_value;    // Changed: that field's values; SameRequest: the outputs
  std::vector<ws::FileChange> workspace_before;  // files that already differed before this call
};

struct RunDiff {
  std::vector<CallDiff> calls;
  std::size_t first_divergence = SIZE_MAX;  // index into calls
  std::vector<ws::FileChange> workspace_end;
  bool has_workspace = false;
  std::uint64_t a_tokens = 0, b_tokens = 0, a_model_us = 0, b_model_us = 0;
  std::size_t same = 0;
};

[[nodiscard]] RunDiff diff_runs(const Recording& a, const Recording& b);
[[nodiscard]] const char* kind_name(CallDiffKind k);

}  // namespace khonsu::rec
