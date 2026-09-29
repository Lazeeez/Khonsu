#include "khonsu/model.hpp"

#include <array>

namespace khonsu {

Digest128 LlmRequest::hash() const {
  DigestBuilder h(domain::kRequest);
  h.bytes(as_bytes(role)).bytes(as_bytes(prompt)).u64(sample);
  return h.finish();
}

bool demo_api_exists(std::string_view fn) { return fn == "fetchUser" || fn == "fetchOrders"; }

namespace {

std::uint32_t tokens_of(std::string_view s) { return static_cast<std::uint32_t>(s.size() / 4 + 1); }

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

std::string planner(const LlmRequest& req, Xoshiro256& rng) {
  (void)req;
  // ~40% of samples decide to read the docs first — the "good" trajectory.
  if (rng.below(100) < 40) {
    return "PLAN v1 [route: research]\n"
           "1. Researcher: read docs/API.md and summarize the user-lookup API.\n"
           "2. Coder: implement getUserName(id) in src/user.ts using that API.\n"
           "3. Coder: register the endpoint in src/generated/client.ts.\n"
           "4. Tester: run the test suite.\n"
           "5. Reviewer: review and write the changelog entry.";
  }
  return "PLAN v1 [route: direct]\n"
         "1. Coder: implement getUserName(id) in src/user.ts (the user API is straightforward).\n"
         "2. Coder: register the endpoint in src/generated/client.ts.\n"
         "3. Tester: run the test suite.\n"
         "4. Reviewer: review and write the changelog entry.";
}

std::string researcher(const LlmRequest& req) {
  if (contains(req.prompt, "fetchUser(id: string)"))
    return "NOTES: The user API is `api.fetchUser(id: string): Promise<User>` exported from src/api.ts. "
           "There is no getUserById. User has fields { id, name, email }.";
  return "NOTES: Documentation did not mention a user lookup API.";
}

std::string coder(const LlmRequest& req, Xoshiro256& rng) {
  static constexpr std::array<std::string_view, 4> kHallucinations = {"getUserById", "fetchUserById", "loadUser",
                                                                      "getUser"};
  const bool knows_api = contains(req.prompt, "fetchUser(");
  std::string_view fn;
  if (knows_api) {
    fn = rng.below(100) < 95 ? "fetchUser" : "fetchUsr";  // rare typo even with good context
  } else {
    fn = kHallucinations[static_cast<std::size_t>(rng.below(kHallucinations.size()))];  // plausible, confident, and wrong
  }
  std::string code =
      "import * as api from \"./api\";\n"
      "\n"
      "/** Returns the display name for a user id. */\n"
      "export async function getUserName(id: string): Promise<string> {\n"
      "  const user = await api.";
  code += fn;
  code +=
      "(id);\n"
      "  return user.name;\n"
      "}\n";
  return "FILE src/user.ts\n" + code + "ENDPOINT " + std::string(fn) + "\n";
}

std::string reviewer(const LlmRequest& req) {
  (void)req;
  return "LGTM. getUserName delegates to the documented API and the suite passes.\n"
         "CHANGELOG: Added getUserName(id) (uses api.fetchUser).";
}

}  // namespace

LlmResponse SimulatedModel::complete(const LlmRequest& req, std::uint64_t entropy) {
  Xoshiro256 rng(entropy ^ req.hash().lo);
  LlmResponse r;
  if (req.role == "planner") r.text = planner(req, rng);
  else if (req.role == "researcher") r.text = researcher(req);
  else if (req.role == "coder") r.text = coder(req, rng);
  else r.text = reviewer(req);
  r.prompt_tokens = tokens_of(req.prompt);
  r.completion_tokens = tokens_of(r.text);
  // ~350 ms TTFT + ~12 ms/token, ±25% jitter.
  const std::uint64_t base = 350'000 + std::uint64_t{r.completion_tokens} * 12'000;
  r.latency_us = base * (75 + rng.below(51)) / 100;
  return r;
}

}  // namespace khonsu
