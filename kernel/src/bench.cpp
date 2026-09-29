#include "khonsu/bench.hpp"

#include "khonsu/json.hpp"
#include "khonsu/kernel.hpp"
#include "khonsu/log.hpp"
#include "khonsu/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <type_traits>
#include <unordered_map>

namespace khonsu::bench {

namespace {

using Clock = std::chrono::steady_clock;

double ns_since(Clock::time_point t0) { return std::chrono::duration<double, std::nano>(Clock::now() - t0).count(); }

// Runs `f` until at least `min_ms` elapsed (and >= 3 times); returns mean ns per call.
// If `f` returns a double, that value is used as the call's duration (to exclude setup).
template <class F>
double per_op_ns(F&& f, double min_ms = 25.0) {
  double total = 0;
  int iters = 0;
  const auto start = Clock::now();
  do {
    const auto t0 = Clock::now();
    if constexpr (std::is_same_v<decltype(f()), double>) {
      total += f();
    } else {
      f();
      total += ns_since(t0);
    }
    ++iters;
  } while (ns_since(start) < min_ms * 1e6 || iters < 3);
  return total / iters;
}

template <class T>
void keep(T&& v) {  // defeat dead-code elimination
  asm volatile("" : : "r"(&v) : "memory");
}

// ---- Strategy 1: deep copy (what "checkpoint = serialize the state" costs) ------------------

struct DeepVfs {
  std::map<std::string, std::vector<std::byte>> files;
};

// ---- Strategy 2: the v1 design — per-page shared_ptr page tables -----------------------------

struct V1Page {
  std::array<std::byte, kPageSize> data;
};
struct V1PageTable {
  std::uint32_t page_id = 0;
  std::shared_ptr<const V1Page> shared_page;
  bool is_dirty = false;
  V1Page* writable() {
    if (shared_page.use_count() > 1) {
      shared_page = std::make_shared<V1Page>(*shared_page);
      is_dirty = true;
    }
    return const_cast<V1Page*>(shared_page.get());
  }
};
struct V1Vfs {
  std::unordered_map<std::string, std::vector<V1PageTable>> manifest;
  [[nodiscard]] V1Vfs fork() const { return V1Vfs{manifest}; }  // O(files + pages)
};

std::string file_name(std::uint32_t i) { return "/data/file-" + std::to_string(i) + ".bin"; }

}  // namespace

std::string run_all(const Config& cfg, const std::function<void(const std::string&)>& progress) {
  auto say = [&](const std::string& s) {
    if (progress) progress(s);
  };
  Json j;
  j.begin_object();
#if defined(__EMSCRIPTEN__)
  j.field("target", "wasm32");
#else
  j.field("target", "native");
#endif

  // ============================ fork cost vs state size ======================================
  j.key("fork").begin_array();
  for (const std::uint32_t total_pages : cfg.state_pages) {
    const std::uint32_t per_file = std::max<std::uint32_t>(1, total_pages / cfg.files);
    std::vector<std::byte> page(kPageSize);
    for (std::size_t i = 0; i < page.size(); ++i) page[i] = static_cast<std::byte>(i * 31);

    // Build identical content in all three representations.
    NodeStore store;
    NodeRef root = kEmpty;
    V1Vfs v1;
    DeepVfs deep;
    for (std::uint32_t f = 0; f < cfg.files; ++f) {
      const std::string name = file_name(f);
      std::vector<std::byte> content(static_cast<std::size_t>(per_file) * kPageSize);
      for (std::uint32_t p = 0; p < per_file; ++p) {
        page[0] = static_cast<std::byte>(f);
        page[1] = static_cast<std::byte>(p);
        std::copy(page.begin(), page.end(), content.begin() + static_cast<std::ptrdiff_t>(p) * kPageSize);
      }
      NodeRef file = store.file_write(store.empty_file(), 0, content);
      root = store.put(root, name, file);
      auto& tables = v1.manifest[name];
      for (std::uint32_t p = 0; p < per_file; ++p) {
        auto pg = std::make_shared<V1Page>();
        std::copy_n(content.begin() + static_cast<std::ptrdiff_t>(p) * kPageSize, kPageSize, pg->data.begin());
        tables.push_back(V1PageTable{p, std::move(pg), false});
      }
      if (cfg.include_deep_copy) deep.files[name] = std::move(content);
    }
    (void)store.digest(root);

    // Batched, adaptive timing: browsers without cross-origin isolation coarsen
    // performance.now() to ~100 us, so every figure is total time / operations.
    constexpr std::uint32_t kWrites = 32;  // first writes to distinct pages after one fork
    Xoshiro256 rng(total_pages);
    std::vector<std::pair<std::string, std::uint64_t>> targets;
    for (std::uint32_t i = 0; i < kWrites; ++i)
      targets.emplace_back(file_name(static_cast<std::uint32_t>(rng.below(cfg.files))),
                           rng.below(per_file) * kPageSize + rng.below(kPageSize));
    const std::byte one{0x5a};

    // Khonsu: fork == append the root handle to a new timeline's step index.
    std::vector<NodeRef> heads;
    heads.reserve(1024);
    const double k_fork = per_op_ns([&] {
      heads.clear();
      for (int i = 0; i < 1024; ++i) heads.push_back(root);
      keep(heads);
    }) / 1024.0;
    const double k_write = per_op_ns([&] {
      NodeRef child = root;  // a fresh fork
      for (const auto& [name, off] : targets)
        child = store.put(child, name, store.file_write(store.get(child, name), off, {&one, 1}));
      keep(child);
    }) / kWrites;
    const std::vector<NodeRef> keep_root = {root};
    store.collect(keep_root);  // drop the benchmark's garbage between sizes

    // v1: copy the page-table manifest (one atomic increment per page) + teardown.
    const double v1_fork = per_op_ns([&] {
      V1Vfs child = v1.fork();
      keep(child);
    });
    const double v1_write = per_op_ns([&] {
      V1Vfs child = v1.fork();
      const auto t0 = Clock::now();
      for (const auto& [name, off] : targets)
        child.manifest[name][static_cast<std::size_t>(off / kPageSize)].writable()->data[static_cast<std::size_t>(off % kPageSize)] = one;
      keep(child);
      return ns_since(t0);
    }) / kWrites;

    double d_fork = 0, d_write = 0;
    if (cfg.include_deep_copy) {
      d_fork = per_op_ns([&] {
        DeepVfs child = deep;
        keep(child);
      });
      DeepVfs child = deep;
      d_write = per_op_ns([&] {
        for (const auto& [name, off] : targets) child.files[name][static_cast<std::size_t>(off)] = one;
        keep(child);
      }) / kWrites;
    }

    j.begin_object();
    j.field("pages", total_pages).field("mib", static_cast<double>(total_pages) * kPageSize / (1 << 20));
    j.field("khonsu_fork_ns", k_fork).field("khonsu_first_write_ns", k_write);
    j.field("v1_fork_ns", v1_fork).field("v1_first_write_ns", v1_write);
    if (cfg.include_deep_copy) j.field("deep_fork_ns", d_fork).field("deep_first_write_ns", d_write);
    j.end_object();
    say("fork: " + std::to_string(total_pages) + " pages done");
  }
  j.end_array();

  // ============================ kernel-level fork on a live scenario =========================
  {
    Kernel k(kDemoSeed);
    const auto main = k.load_scenario("issue-42");
    k.run(main, 200);
    const std::uint32_t head = k.timeline(main).head_step();
    std::uint32_t r = 0;
    const double fork_ns = per_op_ns([&] {
      for (int i = 0; i < 64; ++i) k.fork(main, r++ % (head + 1));
    }, 10.0) / 64.0;
    const auto t0 = Clock::now();
    const auto rep = k.verify(main);
    const double verify_ns = ns_since(t0);
    j.key("kernel").begin_object();
    j.field("fork_ns", fork_ns).field("timelines_created", k.timeline_count() - 1);
    j.field("verify_steps", rep.steps_checked).field("verify_us_per_step", verify_ns / 1000.0 / std::max(1u, rep.steps_checked));
    j.field("avg_step_us", k.metrics().total_step_us / static_cast<double>(std::max<std::uint64_t>(1, k.metrics().steps_executed)));
    j.end_object();
    say("kernel fork/verify done");
  }

  // ============================ event log & CRC32C throughput ================================
  {
    std::vector<std::byte> payload(64);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i);
    EventLog log;
    auto t0 = Clock::now();
    for (std::uint32_t i = 0; i < cfg.log_frames; ++i) log.append(FrameType::OracleResponse, 1, i, payload);
    const double append_ns = ns_since(t0);

    std::vector<std::byte> buf(static_cast<std::size_t>(cfg.crc_mib) << 20);
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<std::byte>(i * 7 + (i >> 9));
    t0 = Clock::now();
    const std::uint32_t c = crc32c(buf);
    keep(c);
    const double crc_ns = ns_since(t0);

    j.key("log").begin_object();
    j.field("frames", cfg.log_frames).field("payload_bytes", payload.size());
    j.field("append_mframes_per_s", cfg.log_frames / (append_ns / 1e9) / 1e6);
    j.field("append_ns_per_frame", append_ns / cfg.log_frames);
    j.field("crc32c_gib_per_s", static_cast<double>(buf.size()) / (crc_ns / 1e9) / (1u << 30));
    j.end_object();
    say("log/crc done");
  }

  j.end_object();
  return j.take();
}

}  // namespace khonsu::bench
