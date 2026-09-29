#include "check.hpp"
#include "khonsu/kernel.hpp"

#include <cstdio>

using namespace khonsu;

namespace {
constexpr std::uint64_t kSeed = kDemoSeed;

std::uint32_t find_tag(const Timeline& t, std::string_view tag) {
  for (std::size_t i = 0; i < t.steps.size(); ++i)
    if (t.steps[i].tag == tag) return t.base_step + static_cast<std::uint32_t>(i);
  return UINT32_MAX;
}

void print(const Kernel& k, std::uint32_t tid) {
  const Timeline& t = k.timeline(tid);
  for (std::size_t i = 0; i < t.steps.size(); ++i) {
    const auto& s = t.steps[i];
    std::printf("      %3u %-10s %8.3fs %-14s %s\n", t.base_step + static_cast<unsigned>(i),
                std::string(actor_name(s.actor)).c_str(), static_cast<double>(s.vtime) / 1e6, s.tag.c_str(),
                s.label.c_str());
  }
}
}  // namespace

TEST("kernel: canned scenario runs to completion and hallucinates on the direct route") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  const auto n = k.run(main, 200);
  CHECK(n > 5 && n < 64);
  CHECK(k.status(main) == "failed");
  CHECK(find_tag(k.timeline(main), "plan_direct") != UINT32_MAX);
  CHECK(find_tag(k.timeline(main), "hallucination") != UINT32_MAX);
  if (std::getenv("KHONSU_VERBOSE")) print(k, main);
}

TEST("kernel: verify() replays bit-exactly from the log") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const auto rep = k.verify(main);
  CHECK(rep.ok);
  CHECK(rep.steps_checked == k.timeline(main).steps.size() - 1);
}

TEST("kernel: fork is O(1) and shares the parent's root") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const auto before = k.store().stats();
  const auto b = k.fork(main, 3);
  const auto after = k.store().stats();
  CHECK(k.timeline(b).steps[0].root == k.root_at(main, 3));
  CHECK(before.branches == after.branches && before.pages == after.pages);  // zero nodes allocated
  CHECK(k.timeline(b).head_step() == 3);
  CHECK(k.root_at(b, 1) == k.root_at(main, 1));  // history below the fork resolves via the parent
}

TEST("kernel: unedited fork replays from the oracle cache and converges to the same state") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const auto b = k.fork(main, 0);
  k.run(b, 200);
  const auto& tb = k.timeline(b);
  CHECK(tb.llm_calls > 0);
  CHECK(tb.cache_hits == tb.llm_calls);  // every request byte-identical ⇒ zero tokens spent
  CHECK(tb.tokens_saved == tb.tokens);
  CHECK(tb.steps.back().digest == k.timeline(main).steps.back().digest);
}

TEST("kernel: fork + injected hint fixes the hallucination (replay-then-live)") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const auto b = k.fork(main, 0);
  k.inject(b, "@coder/notes", "Use api.fetchUser(id) from src/api.ts - there is no getUserById.");
  k.run(b, 200);
  const auto& tb = k.timeline(b);
  CHECK(k.status(b) == "done");
  CHECK(tb.cache_hits >= 1);             // the planner's request is unchanged → served from cache
  CHECK(tb.cache_hits < tb.llm_calls);   // the coder's request changed → went live
  const auto vr = k.verify(b);
  CHECK(vr.ok);
  if (!vr.ok) std::printf("      verify: step %lld: %s\n", static_cast<long long>(vr.first_divergent_step), vr.reason.c_str());
  const auto diff = k.store().diff(k.timeline(main).steps.back().root, tb.steps.back().root);
  bool user_changed = false;
  for (const auto& c : diff) user_changed |= c.key == "/repo/src/user.ts";
  CHECK(user_changed);

  // CoW: the patched generated client shares all but one page with main's version.
  const auto pm = k.store().file_pages(k.store().get(k.timeline(main).steps.back().root, "/repo/src/generated/client.ts"));
  const auto pb = k.store().file_pages(k.store().get(tb.steps.back().root, "/repo/src/generated/client.ts"));
  REQUIRE(pm.size() == pb.size());
  std::size_t shared = 0;
  for (std::size_t i = 0; i < pm.size(); ++i) shared += pm[i].page == pb[i].page;
  CHECK(pm.size() >= 10);
  CHECK(shared == pm.size() - 1);
}

TEST("kernel: tampered oracle response is caught by replay (CRC was fixed up)") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const auto target = find_tag(k.timeline(main), "hallucination") - 1;  // the coder's LLM step
  bool tampered = false;
  for (std::uint32_t g = target; g >= 1 && !tampered; --g) tampered = k.tamper_oracle(main, g);
  REQUIRE(tampered);
  const auto rep = k.verify(main);
  CHECK(!rep.ok);
  CHECK(rep.first_divergent_step >= 1);
  CHECK(rep.reason.find("mismatch") != std::string::npos);
  if (std::getenv("KHONSU_VERBOSE")) std::printf("      %s\n", rep.reason.c_str());
}

TEST("kernel: bit-rot in the log is caught by CRC32C") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  REQUIRE(k.corrupt_frame(main, 4));
  const auto rep = k.verify(main);
  CHECK(!rep.ok);
  CHECK(rep.first_divergent_step == 4);
  CHECK(rep.reason.find("CRC32C") != std::string::npos);
}

TEST("kernel: export → import reproduces the run in a fresh kernel with different entropy") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const auto bytes = k.export_log(main);
  CHECK(!bytes.empty());

  Kernel other(999);  // different host entropy: a live run here would differ
  std::string err;
  const auto imported = other.import_log(bytes, &err);
  CHECK(err.empty());
  REQUIRE(imported != UINT32_MAX);
  CHECK(other.timeline(imported).steps.size() == k.timeline(main).steps.size());
  CHECK(other.timeline(imported).steps.back().digest == k.timeline(main).steps.back().digest);
  CHECK(other.verify(imported).ok);

  const auto live = other.load_scenario("issue-42");
  other.run(live, 200);
  (void)live;  // (may or may not match: that is the point of recording)
}

TEST("kernel: speculative search finds a passing trajectory and prunes the rest") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const auto ids = k.speculate(main, 0, 8, 64);
  REQUIRE(ids.size() == 8);
  CHECK(k.status(ids[0]) == "done");
  CHECK(!k.timeline(ids[0]).pruned);
  for (std::size_t i = 1; i < ids.size(); ++i) CHECK(k.timeline(ids[i]).pruned);
  CHECK(k.verify(ids[0]).ok);
}

TEST("kernel: chaos — killed actor is restarted by the supervisor and the run still verifies") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 3);
  k.kill(main, kCoder);
  k.run(main, 200);
  const auto& t = k.timeline(main);
  CHECK(find_tag(t, "fault") != UINT32_MAX);
  CHECK(find_tag(t, "restart") != UINT32_MAX);
  CHECK(k.status(main) == "failed" || k.status(main) == "done");
  CHECK(k.verify(main).ok);
}

TEST("kernel: gc releases pruned history; surviving timelines still verify") {
  Kernel k(kSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const auto ids = k.speculate(main, 0, 6, 64);
  const auto before = k.store().stats();
  const auto freed = k.gc();
  const auto after = k.store().stats();
  CHECK(freed > 0);
  CHECK(after.bytes < before.bytes);
  CHECK(k.verify(main).ok);
  CHECK(k.verify(ids[0]).ok);
}

TEST("kernel: many entropy seeds — every run verifies (nondeterminism is fully recorded)") {
  int done = 0, failed = 0;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Kernel k(seed);
    const auto main = k.load_scenario("issue-42");
    k.run(main, 200);
    CHECK(k.verify(main).ok);
    (k.status(main) == "done" ? done : failed)++;
  }
  CHECK(done > 0 && failed > 0);  // both trajectories occur across seeds
}
