// Khonsu — the deterministic agent kernel.
//
//  * Every piece of agent-world state — files, agent memory, *and the scheduler's
//    pending-event queue* — lives in one persistent Merkle tree. A step is a
//    transaction: pop the next event in total (vtime, seq) order, run the target
//    agent's reducer, commit a new root. fork() is therefore a root-handle copy.
//  * The Oracle is the only door for nondeterminism. Live: consult the response
//    cache (keyed by request hash), else the model; record both request and
//    response frames. Replay: serve recorded responses and assert request hashes.
//  * verify() re-executes a timeline from its base and compares the Merkle digest
//    at every commit, reporting the first divergent step and a key-level diff.
#pragma once

#include "khonsu/common.hpp"
#include "khonsu/log.hpp"
#include "khonsu/model.hpp"
#include "khonsu/store.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace khonsu {

// Host-entropy seed of the canned demo run (planner skips the docs → hallucination).
inline constexpr std::uint64_t kDemoSeed = 1;

enum Actor : std::uint16_t {
  kUser = 0,
  kPlanner = 1,
  kResearcher = 2,
  kCoder = 3,
  kTester = 4,
  kReviewer = 5,
  kSupervisor = 6,
  kActorCount = 7,
};
[[nodiscard]] std::string_view actor_name(std::uint16_t a) noexcept;

enum class EventKind : std::uint8_t { Start = 0, Msg = 1, LlmReply = 2, ToolReply = 3, Restart = 4 };

struct Event {
  std::uint64_t vtime = 0;
  std::uint64_t seq = 0;
  std::uint16_t to = 0, from = 0;
  EventKind kind = EventKind::Msg;
  std::string topic, body;
};

enum class StepKind : std::uint8_t { Genesis = 0, Dispatch = 1, UserEdit = 2, Fault = 3 };

enum class SpanKind : std::uint8_t { Step = 0, Llm = 1, Tool = 2 };

struct Span {
  std::uint16_t actor = 0;
  SpanKind kind = SpanKind::Step;
  bool cached = false;
  std::uint32_t step = 0;  // global step that opened the span
  std::uint64_t start = 0, end = 0;
  std::string name;
};

struct StepRecord {
  NodeRef root;
  Digest128 digest, chain;
  std::uint64_t lsn_begin = 0, lsn_end = 0;  // frames of this step in the timeline's log
  std::uint64_t vtime = 0;
  std::uint16_t actor = 0;
  StepKind kind = StepKind::Genesis;
  std::uint32_t tokens = 0, tokens_saved = 0;
  double wall_us = 0;  // measured host time (observability only; outside determinism)
  std::string label, tag;
};

struct Timeline {
  std::uint32_t id = 0;
  std::int32_t parent = -1;
  std::uint32_t base_step = 0;  // global step index of steps[0]
  std::uint64_t sample = 0;     // sampling salt for LLM requests
  std::string name;
  bool pruned = false;
  EventLog log;
  std::vector<StepRecord> steps;  // steps[i] <-> global step base_step + i
  std::vector<Span> spans;
  std::uint64_t tokens = 0, tokens_saved = 0, llm_calls = 0, cache_hits = 0;

  [[nodiscard]] std::uint32_t head_step() const { return base_step + static_cast<std::uint32_t>(steps.size()) - 1; }
};

struct VerifyReport {
  bool ok = true;
  std::uint32_t steps_checked = 0;
  std::int64_t first_divergent_step = -1;
  std::string reason;
  std::vector<Change> diff;  // recorded root vs replayed root at the divergent step
  double ms = 0;
};

struct KernelMetrics {
  double last_fork_us = 0;
  double total_step_us = 0;
  std::uint64_t steps_executed = 0;
  double last_verify_ms = 0;
  std::uint32_t last_verify_steps = 0;
  double last_gc_ms = 0;
  std::uint64_t last_gc_freed = 0;
};

struct DigestHash {
  std::size_t operator()(const Digest128& d) const noexcept { return static_cast<std::size_t>(d.lo ^ (d.hi >> 1)); }
};

class OracleSource;
struct StepCtx;

class Kernel {
public:
  explicit Kernel(std::uint64_t entropy_seed, std::unique_ptr<LanguageModel> model = nullptr);
  ~Kernel();

  // Creates the genesis timeline for a scenario ("issue-42"). Returns its id.
  std::uint32_t load_scenario(std::string_view name);

  bool step(std::uint32_t tid);                           // false when idle/finished
  std::uint32_t run(std::uint32_t tid, std::uint32_t max_steps);
  std::uint32_t fork(std::uint32_t tid, std::uint32_t global_step, std::string name = {},
                     std::optional<std::uint64_t> sample = std::nullopt);
  void inject(std::uint32_t tid, std::string_view key, std::string_view text);  // at head
  void kill(std::uint32_t tid, std::uint16_t actor);                             // at head
  // Fork `n` sampled branches at `global_step`, run each to completion, score them.
  // Returns ids ordered best-first; losers are marked pruned.
  std::vector<std::uint32_t> speculate(std::uint32_t tid, std::uint32_t global_step, std::uint32_t n,
                                       std::uint32_t max_steps = 64);
  VerifyReport verify(std::uint32_t tid);

  // Adversarial tests for the verifier.
  bool tamper_oracle(std::uint32_t tid, std::uint32_t global_step);  // edits a response, fixes CRC
  bool corrupt_frame(std::uint32_t tid, std::uint32_t global_step);  // flips a bit, CRC left stale

  // Recorded traces (.khn): export a genesis-rooted timeline, import by verified replay.
  [[nodiscard]] std::vector<std::byte> export_log(std::uint32_t tid) const;
  std::uint32_t import_log(std::span<const std::byte> bytes, std::string* error = nullptr);

  std::size_t gc();
  void prune(std::uint32_t tid);

  // ---- queries -----------------------------------------------------------------------
  [[nodiscard]] std::size_t timeline_count() const { return timelines_.size(); }
  [[nodiscard]] const Timeline& timeline(std::uint32_t tid) const { return *timelines_.at(tid); }
  [[nodiscard]] NodeRef root_at(std::uint32_t tid, std::uint32_t global_step) const;
  [[nodiscard]] const StepRecord& record_at(std::uint32_t tid, std::uint32_t global_step) const;
  [[nodiscard]] std::string status(std::uint32_t tid) const;  // running | done | failed
  [[nodiscard]] std::vector<Event> pending(NodeRef root) const;
  [[nodiscard]] NodeStore& store() { return store_; }
  [[nodiscard]] const NodeStore& store() const { return store_; }
  [[nodiscard]] const KernelMetrics& metrics() const { return metrics_; }
  [[nodiscard]] std::string_view model_name() const { return model_->name(); }
  [[nodiscard]] std::uint64_t cache_size() const { return cache_.size(); }

private:
  friend struct StepCtx;
  friend class LiveOracle;

  struct StepInput {
    StepKind kind = StepKind::Dispatch;
    std::string key, text;  // UserEdit
    std::uint16_t actor = 0;  // Fault
  };
  struct StepOutput {
    NodeRef root;
    std::uint64_t vtime = 0;
    std::uint16_t actor = 0;
    std::string label, tag;
    std::vector<Span> spans;
    std::uint32_t tokens = 0, tokens_saved = 0, llm_calls = 0, cache_hits = 0;
    bool idle = false;
  };

  StepOutput apply(NodeRef root, std::uint32_t global_step, std::uint64_t sample, const StepInput& in,
                   OracleSource& oracle);
  void dispatch(StepCtx& ctx);
  void commit(Timeline& t, const StepInput& in, StepOutput&& out, std::uint64_t lsn_begin, double wall_us);
  bool execute(std::uint32_t tid, const StepInput& in);
  Timeline& tl(std::uint32_t tid) { return *timelines_.at(tid); }

  // Queue persistence inside the world state.
  std::vector<Event> load_queue(NodeRef root) const;
  NodeRef save_queue(NodeRef root, const std::vector<Event>& q);

  NodeStore store_;
  std::unique_ptr<LanguageModel> model_;
  Xoshiro256 entropy_;  // host entropy: NOT part of the determinism boundary
  std::unordered_map<Digest128, LlmResponse, DigestHash> cache_;  // lookup only; never iterated
  std::vector<std::unique_ptr<Timeline>> timelines_;
  std::string scenario_;
  std::uint64_t scenario_seed_ = 0;
  KernelMetrics metrics_;
};


}  // namespace khonsu
