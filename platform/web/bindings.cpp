// Khonsu — WebAssembly bridge.
//
// A flat C ABI (no embind: lower per-call overhead, no RTTI) called from the
// kernel Web Worker. Cold-path queries return JSON in a buffer that stays valid
// until the next call; JS copies it out immediately. Every entry point is
// exception-safe: failures come back as {"error": "..."}.
#include "khonsu/bench.hpp"
#include "khonsu/json.hpp"
#include "khonsu/kernel.hpp"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>

using namespace khonsu;

namespace {

std::unique_ptr<Kernel> g_kernel;
std::string g_out;
std::vector<std::byte> g_blob;

const char* emit(std::string s) {
  g_out = std::move(s);
  return g_out.c_str();
}

const char* error(const std::string& what) {
  Json j;
  j.begin_object().field("error", what).end_object();
  return emit(j.take());
}

template <class F>
const char* guarded(F&& f) {
  try {
    if (!g_kernel) return error("kernel not initialised");
    return f();
  } catch (const std::exception& e) {
    return error(e.what());
  }
}

std::string_view kind_name(StepKind k) {
  switch (k) {
    case StepKind::Genesis: return "genesis";
    case StepKind::Dispatch: return "dispatch";
    case StepKind::UserEdit: return "edit";
    case StepKind::Fault: return "fault";
  }
  return "?";
}

std::string_view span_kind(SpanKind k) {
  switch (k) {
    case SpanKind::Step: return "step";
    case SpanKind::Llm: return "llm";
    case SpanKind::Tool: return "tool";
  }
  return "?";
}

std::string preview(std::string_view s, std::size_t n) {
  if (s.size() <= n) return std::string(s);
  return std::string(s.substr(0, n)) + "…";
}

void write_stats(Json& j) {
  const auto s = g_kernel->store().stats();
  const auto& m = g_kernel->metrics();
  j.key("store").begin_object();
  j.field("branches", s.branches).field("entries", s.entries).field("blobs", s.blobs).field("pages", s.pages);
  j.field("files", s.files).field("bytes", s.bytes).field("pages_allocated", s.pages_allocated);
  j.field("page_bytes_copied", s.page_bytes_copied);
  j.end_object();
  j.key("metrics").begin_object();
  j.field("last_fork_us", m.last_fork_us).field("steps_executed", m.steps_executed);
  j.field("avg_step_us", m.steps_executed ? m.total_step_us / static_cast<double>(m.steps_executed) : 0.0);
  j.field("last_verify_ms", m.last_verify_ms).field("last_verify_steps", m.last_verify_steps);
  j.field("last_gc_ms", m.last_gc_ms).field("last_gc_freed", m.last_gc_freed);
  j.end_object();
  j.field("model", g_kernel->model_name()).field("cache_entries", g_kernel->cache_size());
  j.field("heap_bytes", static_cast<std::uint64_t>(EM_ASM_DOUBLE({ return HEAP8.length; })));
}

void write_timeline(Json& j, const Timeline& t) {
  j.begin_object();
  j.field("id", t.id).field("parent", t.parent).field("base", t.base_step).field("name", t.name);
  j.field("sample", std::to_string(t.sample)).field("pruned", t.pruned).field("status", g_kernel->status(t.id));
  j.field("head", t.head_step()).field("tokens", t.tokens).field("tokens_saved", t.tokens_saved);
  j.field("llm_calls", t.llm_calls).field("cache_hits", t.cache_hits).field("log_frames", t.log.size());
  j.field("log_bytes", t.log.bytes());
  j.key("steps").begin_array();
  for (std::size_t i = 0; i < t.steps.size(); ++i) {
    const StepRecord& s = t.steps[i];
    j.begin_object();
    j.field("g", t.base_step + static_cast<std::uint32_t>(i)).field("actor", s.actor).field("kind", kind_name(s.kind));
    j.field("tag", s.tag).field("label", s.label).field("vtime", s.vtime).field("digest", s.digest.hex());
    j.field("tokens", s.tokens).field("saved", s.tokens_saved).field("wall_us", s.wall_us);
    j.field("lsn0", s.lsn_begin).field("lsn1", s.lsn_end).field("live", !s.root.empty());
    j.end_object();
  }
  j.end_array();
  j.key("spans").begin_array();
  for (const Span& s : t.spans) {
    j.begin_object();
    j.field("a", s.actor).field("k", span_kind(s.kind)).field("c", s.cached).field("g", s.step);
    j.field("t0", s.start).field("t1", s.end).field("n", s.name);
    j.end_object();
  }
  j.end_array();
  j.end_object();
}

std::string frame_json(const EventLog& log, std::uint64_t lsn) {
  const FrameView f = log.frame(lsn);
  Json j;
  j.begin_object();
  j.field("lsn", lsn).field("type", frame_type_name(f.header.type)).field("actor", f.header.actor);
  j.field("vtime", f.header.vtime_us).field("bytes", 32 + ((f.header.payload_len + 7) & ~7u));
  j.field("crc", std::to_string(f.header.crc32c)).field("crc_ok", log.verify(lsn));
  Reader rd(f.payload);
  switch (f.header.type) {
    case FrameType::OracleRequest: {
      j.field("hash", rd.digest().hex());
      j.field("role", rd.str());
      j.field("sample", std::to_string(rd.varint()));
      j.field("prompt", preview(rd.str(), 4000));
      break;
    }
    case FrameType::OracleResponse: {
      j.field("hash", rd.digest().hex());
      j.field("cached", rd.u8() != 0);
      j.field("latency_us", rd.varint());
      j.field("prompt_tokens", rd.varint());
      j.field("completion_tokens", rd.varint());
      j.field("text", preview(rd.str(), 4000));
      break;
    }
    case FrameType::StepCommit: {
      j.field("step", rd.varint());
      j.field("state", rd.digest().hex());
      j.field("chain", rd.digest().hex());
      break;
    }
    case FrameType::StepBegin: {
      j.field("step", rd.varint());
      j.field("kind", rd.u8());
      break;
    }
    case FrameType::Fork: {
      j.field("parent", rd.varint());
      j.field("step", rd.varint());
      j.field("state", rd.digest().hex());
      break;
    }
    case FrameType::Genesis: {
      j.field("scenario", rd.str());
      break;
    }
    default:
      break;
  }
  j.end_object();
  return j.take();
}

bool is_text(std::string_view s) {
  return std::none_of(s.begin(), s.end(), [](char c) { return c == '\0'; });
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int kh_init(double seed) {
  g_kernel = std::make_unique<Kernel>(static_cast<std::uint64_t>(seed));
  return static_cast<int>(g_kernel->load_scenario("issue-42"));
}

// Amortized cost of Kernel::fork in µs, measured on a scratch kernel. Browsers
// coarsen timers (100 µs without cross-origin isolation), so one fork can't be timed.
EMSCRIPTEN_KEEPALIVE double kh_fork_cost() {
  Kernel k(kDemoSeed);
  const auto main = k.load_scenario("issue-42");
  k.run(main, 200);
  const std::uint32_t head = k.timeline(main).head_step();
  const double t0 = emscripten_get_now();
  constexpr int kN = 4000;
  for (int i = 0; i < kN; ++i) k.fork(main, static_cast<std::uint32_t>(i) % (head + 1));
  return (emscripten_get_now() - t0) * 1000.0 / kN;
}

EMSCRIPTEN_KEEPALIVE double kh_demo_seed() { return static_cast<double>(kDemoSeed); }

EMSCRIPTEN_KEEPALIVE const char* kh_step(int tid, int n) {
  return guarded([&] {
    const auto done = g_kernel->run(static_cast<std::uint32_t>(tid), static_cast<std::uint32_t>(n));
    Json j;
    j.begin_object().field("executed", done).end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_fork(int tid, int step) {
  return guarded([&] {
    const auto id = g_kernel->fork(static_cast<std::uint32_t>(tid), static_cast<std::uint32_t>(step));
    Json j;
    j.begin_object().field("id", id).field("fork_us", g_kernel->metrics().last_fork_us).end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_inject(int tid, const char* key, const char* text) {
  return guarded([&] {
    g_kernel->inject(static_cast<std::uint32_t>(tid), key, text);
    return emit("{}");
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_kill(int tid, int actor) {
  return guarded([&] {
    g_kernel->kill(static_cast<std::uint32_t>(tid), static_cast<std::uint16_t>(actor));
    return emit("{}");
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_speculate(int tid, int step, int n) {
  return guarded([&] {
    const auto ids =
        g_kernel->speculate(static_cast<std::uint32_t>(tid), static_cast<std::uint32_t>(step), static_cast<std::uint32_t>(n));
    Json j;
    j.begin_object().key("ranked").begin_array();
    for (auto id : ids) j.value(id);
    j.end_array().end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_world() {
  return guarded([&] {
    Json j;
    j.begin_object();
    j.key("timelines").begin_array();
    for (std::uint32_t i = 0; i < g_kernel->timeline_count(); ++i) write_timeline(j, g_kernel->timeline(i));
    j.end_array();
    write_stats(j);
    j.end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_stats() {
  return guarded([&] {
    Json j;
    j.begin_object();
    write_stats(j);
    j.end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_state(int tid, int step) {
  return guarded([&] {
    const NodeStore& s = g_kernel->store();
    const NodeRef root = g_kernel->root_at(static_cast<std::uint32_t>(tid), static_cast<std::uint32_t>(step));
    if (root.empty()) return error("history for this step was pruned by GC");
    struct Item {
      std::string key;
      NodeRef v;
    };
    std::vector<Item> items;
    s.for_each(root, [&](std::string_view k, NodeRef v) { items.push_back({std::string(k), v}); });
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
    Json j;
    j.begin_object().field("root", s.digest(root).hex());
    j.key("entries").begin_array();
    for (const auto& it : items) {
      j.begin_object().field("key", it.key).field("digest", s.digest(it.v).hex());
      if (it.v.tag() == Tag::File) {
        j.field("type", "file").field("size", s.file_size(it.v)).field("pages", s.file_pages(it.v).size());
      } else {
        const auto b = s.blob(it.v);
        j.field("type", "blob").field("size", b.size());
        if (it.key == "sys/queue") j.field("preview", "(scheduler queue — see pending)");
        else j.field("preview", is_text(b) ? preview(b, 600) : std::string("(binary)"));
      }
      j.end_object();
    }
    j.end_array();
    j.key("pending").begin_array();
    for (const Event& e : g_kernel->pending(root)) {
      j.begin_object().field("vtime", e.vtime).field("seq", e.seq).field("to", e.to).field("from", e.from);
      j.field("kind", static_cast<int>(e.kind)).field("topic", e.topic).field("body", preview(e.body, 200));
      j.end_object();
    }
    j.end_array().end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_file(int tid, int step, const char* path) {
  return guarded([&] {
    const NodeRef root = g_kernel->root_at(static_cast<std::uint32_t>(tid), static_cast<std::uint32_t>(step));
    const auto content = g_kernel->store().read_file(root, path);
    Json j;
    j.begin_object();
    if (content) j.field("text", *content);
    else j.key("text").null();
    j.end_object();
    return emit(j.take());
  });
}

// CoW page map of `path` at (tid, step), compared page-by-page with (btid, bstep).
EMSCRIPTEN_KEEPALIVE const char* kh_pages(int tid, int step, const char* path, int btid, int bstep) {
  return guarded([&] {
    const NodeStore& s = g_kernel->store();
    const NodeRef a = s.get(g_kernel->root_at(static_cast<std::uint32_t>(tid), static_cast<std::uint32_t>(step)), path);
    const NodeRef b = btid < 0 ? kEmpty
                               : s.get(g_kernel->root_at(static_cast<std::uint32_t>(btid), static_cast<std::uint32_t>(bstep)), path);
    const auto pa = s.file_pages(a);
    const auto pb = s.file_pages(b);
    Json j;
    j.begin_object().field("size", s.file_size(a)).key("pages").begin_array();
    for (const auto& p : pa) {
      const auto it = std::find_if(pb.begin(), pb.end(), [&](const PageSlot& q) { return q.index == p.index; });
      const char* st = it == pb.end() ? "new" : (it->page == p.page ? "shared" : "private");
      j.begin_object().field("i", p.index).field("s", st).field("ref", p.page.raw).field("d", s.digest(p.page).short_hex());
      j.end_object();
    }
    j.end_array().end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_diff(int t1, int s1, int t2, int s2) {
  return guarded([&] {
    const NodeStore& s = g_kernel->store();
    const NodeRef a = g_kernel->root_at(static_cast<std::uint32_t>(t1), static_cast<std::uint32_t>(s1));
    const NodeRef b = g_kernel->root_at(static_cast<std::uint32_t>(t2), static_cast<std::uint32_t>(s2));
    auto text_of = [&](NodeRef v) -> std::string {
      if (v.empty()) return {};
      if (v.tag() == Tag::File) return s.file_read_all(v);
      return std::string(s.blob(v));
    };
    Json j;
    j.begin_object().key("changes").begin_array();
    for (const Change& c : s.diff(a, b)) {
      if (c.key == "sys/queue") continue;
      j.begin_object().field("key", c.key);
      j.field("kind", c.kind == ChangeKind::Added ? "added" : c.kind == ChangeKind::Removed ? "removed" : "modified");
      const std::string before = text_of(c.before), after = text_of(c.after);
      if (before.size() + after.size() < 200'000 && is_text(before) && is_text(after)) {
        j.field("before", before).field("after", after);
      }
      j.end_object();
    }
    j.end_array().end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_frames(int tid, int step) {
  return guarded([&] {
    const Timeline& t = g_kernel->timeline(static_cast<std::uint32_t>(tid));
    const std::uint32_t g = static_cast<std::uint32_t>(step);
    Json j;
    j.begin_object();
    if (g < t.base_step) {
      j.field("inherited", true);
    } else {
      const StepRecord& r = t.steps.at(g - t.base_step);
      j.key("frames").begin_array();
      for (std::uint64_t l = r.lsn_begin; l < r.lsn_end; ++l) j.raw(frame_json(t.log, l));
      j.end_array();
    }
    j.end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_verify(int tid) {
  return guarded([&] {
    const auto r = g_kernel->verify(static_cast<std::uint32_t>(tid));
    Json j;
    j.begin_object().field("ok", r.ok).field("steps", r.steps_checked).field("ms", r.ms);
    j.field("step", r.first_divergent_step).field("reason", r.reason);
    j.key("diff").begin_array();
    for (const auto& c : r.diff) j.value(c.key);  // incl. sys/queue: pending replies live there
    j.end_array().end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE int kh_tamper(int tid, int step) {
  if (!g_kernel) return 0;
  try {
    return g_kernel->tamper_oracle(static_cast<std::uint32_t>(tid), static_cast<std::uint32_t>(step)) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

EMSCRIPTEN_KEEPALIVE int kh_corrupt(int tid, int step) {
  if (!g_kernel) return 0;
  try {
    return g_kernel->corrupt_frame(static_cast<std::uint32_t>(tid), static_cast<std::uint32_t>(step)) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

EMSCRIPTEN_KEEPALIVE const char* kh_gc() {
  return guarded([&] {
    const auto freed = g_kernel->gc();
    Json j;
    j.begin_object().field("freed", freed).field("ms", g_kernel->metrics().last_gc_ms).end_object();
    return emit(j.take());
  });
}

// Export: returns a pointer to the serialized log; length via kh_blob_len().
EMSCRIPTEN_KEEPALIVE const std::byte* kh_export(int tid) {
  if (!g_kernel) return nullptr;
  try {
    g_blob = g_kernel->export_log(static_cast<std::uint32_t>(tid));
  } catch (...) {
    g_blob.clear();
  }
  return g_blob.data();
}

EMSCRIPTEN_KEEPALIVE int kh_blob_len() { return static_cast<int>(g_blob.size()); }

EMSCRIPTEN_KEEPALIVE const char* kh_import(const std::byte* data, int len) {
  return guarded([&] {
    std::string err;
    const auto id = g_kernel->import_log({data, static_cast<std::size_t>(len)}, &err);
    Json j;
    j.begin_object().field("id", static_cast<std::int64_t>(id == UINT32_MAX ? -1 : static_cast<std::int64_t>(id)));
    j.field("error", err).end_object();
    return emit(j.take());
  });
}

EMSCRIPTEN_KEEPALIVE const char* kh_bench(int quick) {
  try {
    bench::Config cfg;
    if (quick) {
      cfg.state_pages = {256, 1024, 4096};
      cfg.reps = 9;
      cfg.log_frames = 100'000;
      cfg.crc_mib = 16;
    }
    return emit(bench::run_all(cfg));
  } catch (const std::exception& e) {
    return error(e.what());
  }
}

}  // extern "C"
