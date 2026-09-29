// Khonsu — simulated language model used by the offline demo scenarios.
//
// It is intentionally *not* a pure function of the prompt: sampling draws on
// host entropy (like temperature > 0), and latency jitters. That is exactly the
// kind of nondeterminism the Oracle must record — replaying without the log
// would produce a different run; replaying with it is bit-exact. Real backends
// (WebLLM, BYOK HTTP APIs) plug in behind the same interface.
#pragma once

#include "khonsu/common.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace khonsu {

struct LlmRequest {
  std::string role;    // planner | researcher | coder | reviewer
  std::string prompt;  // full prompt text
  std::uint64_t sample = 0;  // sampling salt: distinct values ask for distinct samples

  [[nodiscard]] Digest128 hash() const;
};

struct LlmResponse {
  std::string text;
  std::uint32_t prompt_tokens = 0;
  std::uint32_t completion_tokens = 0;
  std::uint64_t latency_us = 0;
};

class LanguageModel {
public:
  virtual ~LanguageModel() = default;
  [[nodiscard]] virtual std::string_view name() const = 0;
  virtual LlmResponse complete(const LlmRequest& req, std::uint64_t entropy) = 0;
};

class SimulatedModel final : public LanguageModel {
public:
  [[nodiscard]] std::string_view name() const override { return "khonsu-sim-1 (offline)"; }
  LlmResponse complete(const LlmRequest& req, std::uint64_t entropy) override;
};

// Ground truth used by scenario tooling (tests, UI annotations): the API names the
// demo repo actually exports.
[[nodiscard]] bool demo_api_exists(std::string_view fn);

}  // namespace khonsu
