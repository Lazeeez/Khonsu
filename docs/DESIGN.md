# Khonsu — Design Document (v2)

> **Khonsu** is a deterministic runtime for multi-agent LLM systems. Every agent step is a transaction over a **persistent, content-addressed state tree**, and every source of nondeterminism enters through one recorded door (the **Oracle**). Together these give you O(1) `fork()` at any step, O(1) seek to any point in history, bit-exact replay with automatic divergence detection, and sandboxed, gas-metered tool execution. It ships as a native C++ engine and as a WebAssembly build that runs fully client-side on GitHub Pages.

This document revises the v1 proposal. **Part I** reviews v1: its bugs, the claims that don't hold up, and the platform problems that would block it. **Part II** is the redesigned architecture. **Part III** covers the web platform, the frontend and performance. **Part IV** covers the build, testing, the roadmap and interview preparation.

---

## Implementation status (v0.1, in this repo)

| Area | Status | Notes |
|---|---|---|
| Persistent Merkle state tree (§3) | ✅ built | 32-way bitmap HAMT, u32 handles into typed pools, slab-allocated child arrays, canonical form, lazy XXH3-128 digests, Merkle diff, mark-sweep GC |
| 4 KiB CoW files | ✅ built | Radix/HAMT over page index. Unchanged pages are kept shared on rewrite (write-if-changed) |
| Reducers + Oracle (§4) | ✅ built | Record / replay / request-hash cache; replay-then-live falls out of the cache |
| Event log (§5) | ✅ built | 32 B frames, CRC32C (HW on x86, slicing-by-8 on wasm), StepCommit(state digest, hash chain), torn-tail recovery, `.khn` export/import-by-verified-replay |
| Timelines (§6) | ✅ built | O(1) fork, O(1) seek, divergence detection with key-level diff. Log-spaced retention not yet |
| Scheduler (§7) | ✅ single-threaded | Total `(vtime, seq)` order; queue lives inside the state. Parallel worker pool / SAB data plane: roadmap M4 |
| Speculative search (§7.3) | ✅ built | Sequential execution of branches for now |
| Simulated cluster / chaos (§7.4) | ◐ partial | Actor kill + supervisor restart + deferred delivery. Network partitions: roadmap |
| Sandbox (§8) | ◐ stub | `run_tests` is a deterministic in-kernel tool over the VFS. QuickJS-wasm + gas instrumentation: roadmap M3 |
| Merge + CRDT cells (§9) | ☐ roadmap M5 | |
| Durability to OPFS (§10) | ☐ roadmap M2 | Log format and recovery are done; the OPFS backend is not |
| Tracing (§11) | ◐ partial | Spans over virtual time in the UI. OTLP/Perfetto export: roadmap |
| Web platform (§12) | ✅ single-threaded build | Worker-hosted kernel, flat C ABI. COI service worker + MT build arrive with M4 |
| Frontend (§14–15) | ✅ built | Multiverse, scrubber, CoW page map, causal trace, inspector (frames / state / diff / verify), guided tour, `/bench`. Canvas 2D (WebGL is an optimization for >10k nodes) |
| LLM backends (§13) | ◐ simulated | `khonsu-sim-1` samples with host entropy. WebLLM / BYOK: roadmap M5 |

**Deviations from the text below (deliberate, for now):** the hash chain uses XXH3-128, not BLAKE3 (to be swapped for tamper-*resistance* rather than tamper-*evidence*). The demo scenario's hallucination lands at step 4, not step 12. Timings measured so far (Node 24 / wasm32): fork ≈ 1 ns store-level and 0.7 µs kernel-level; first write after fork ≈ 1–2 µs per page; CRC32C ≈ 2 GiB/s; log append ≈ 16 M frames/s.

---

## Table of Contents

- [Part I — Review of v1](#part-i--review-of-v1)
- [Part II — Architecture v2](#part-ii--architecture-v2)
  - [1. The core idea](#1-the-core-idea)
  - [2. System overview](#2-system-overview)
  - [3. State model: persistent Merkle state tree](#3-state-model-persistent-merkle-state-tree)
  - [4. Agents as reducers, effects as oracle calls](#4-agents-as-reducers-effects-as-oracle-calls)
  - [5. Event log](#5-event-log)
  - [6. Timelines: fork, seek, replay, divergence](#6-timelines-fork-seek-replay-divergence)
  - [7. Deterministic scheduler and concurrency model](#7-deterministic-scheduler-and-concurrency-model)
  - [8. Sandbox and gas metering](#8-sandbox-and-gas-metering)
  - [9. Merging timelines (and where CRDTs actually belong)](#9-merging-timelines-and-where-crdts-actually-belong)
  - [10. Durability and crash recovery](#10-durability-and-crash-recovery)
  - [11. Tracing and observability](#11-tracing-and-observability)
- [Part III — Platform, Frontend, Performance](#part-iii--platform-frontend-performance)
  - [12. Running on GitHub Pages](#12-running-on-github-pages)
  - [13. LLMs on a static site](#13-llms-on-a-static-site)
  - [14. Frontend architecture](#14-frontend-architecture)
  - [15. Visual design](#15-visual-design)
  - [16. Performance targets and methodology](#16-performance-targets-and-methodology)
- [Part IV — Engineering, Roadmap, Interview Kit](#part-iv--engineering-roadmap-interview-kit)
  - [17. Correctness strategy](#17-correctness-strategy)
  - [18. Repository layout, build and CI](#18-repository-layout-build-and-ci)
  - [19. Roadmap (vertical slices)](#19-roadmap-vertical-slices)
  - [20. Interview kit](#20-interview-kit)
  - [21. Non-goals and cut list](#21-non-goals-and-cut-list)

---

# Part I — Review of v1

v1 has the right ambition: OS primitives applied to agent execution. But several of its central claims won't survive a senior interviewer who has built storage or runtime systems, and two platform assumptions would stop it from running on GitHub Pages at all. The table ranks the issues by how much they matter.

### 1. Blockers

| # | Issue | Why it matters | v2 fix |
|---|---|---|---|
| B1 | **GitHub Pages cannot set `COOP`/`COEP` headers.** `SharedArrayBuffer` and wasm pthreads require `crossOriginIsolated === true`. | The multithreaded WASM build won't start on Pages. | Ship a COI service worker that injects the headers, plus a single-threaded fallback build selected by feature detection (§12). |
| B2 | **The design never defines what state gets forked.** "Pause the process, duplicate the page table" doesn't cover the agent's in-flight program state (call stacks, coroutine frames, pending promises). | You can't fork a running C++ coroutine or a live wasm instance in the browser. So `fork()` only works if all agent state is data. | Agents are **pure reducers** over persistent state. The only fork points are step boundaries, where no in-flight state exists (§4). |
| B3 | **Gas metering as a "decorator in the host execution loop"** | JIT-compiled wasm has no host loop to decorate. The mechanism as described doesn't exist. | Rewrite the wasm bytecode to inject gas accounting, with a watchdog worker as a backstop (§8). |

### 2. False or overstated claims

| Claim in v1 | Reality | v2 |
|---|---|---|
| "Fork is O(1)" (CoW VFS) | Copying `unordered_map<string, vector<CoWPageTable>>` is **O(files + pages)**: one heap allocation per file plus one atomic increment per page. A 1 GiB VFS means about 262k atomic RMWs and 262k × ~32 B of copying per fork. | A persistent radix tree/HAMT: fork copies **one root handle**, which is genuinely O(1). |
| "Time travel = reset to genesis and replay 0..N" | That makes each seek **O(N)**. Scrubbing a 100k-step trace at 60 fps is impossible. | Every step keeps its state root, so seek is **O(1)**. Structural sharing keeps the memory cost small. |
| "100% deterministic replay" | Logging LLM outputs is necessary but not enough. v1 ignores actor **scheduling order**, iteration order of `unordered_map`, libm differences between targets, wasm NaN bit patterns, Relaxed SIMD, and wall-clock timestamps written into frame headers. | Deterministic scheduler, virtual time, a banned-API list, and a cross-target determinism test in CI (§7, §17). |
| "Microsecond logical clocks" | Logical clocks count events, not microseconds, so the phrase contradicts itself. Interviewers will pick up on it. | Virtual time inside the kernel. Wall-clock time enters only as a recorded oracle value. |
| "LWW-Element-Set" | The code implements an **LWW-Register**, which is a different CRDT. | The correct names, and CRDTs used only where they fit (§9). |
| "CRDTs merge divergent timelines" | Forked timelines are **counterfactuals**, not concurrent replicas. LWW merging silently throws away one branch's work, and there's a common ancestor available that it doesn't use. | 3-way merge against the fork-point ancestor. CRDT semantics only for keys declared as mergeable (§9). |
| "Distributed actor mesh" | In a browser tab nothing is distributed, and interviewers will ask about partitions, consensus and membership. | A **deterministic simulated cluster** (FoundationDB-style): simulated network, partitions and message loss, all replayable. Honest and more impressive (§7.4). |
| "Lock-free MPMC queues" for mailboxes | An actor mailbox has many producers and one consumer, so it's **MPSC**. Inside a deterministic timeline you need no lock-free queue at all. | A deterministic priority queue per timeline. Lock-free SPSC/work-stealing structures only *between* threads (§7). |

### 3. Code-level bugs in the v1 sketches

1. `write_file_bytes` **silently truncates** any write that crosses a page boundary. It writes `min(len, PAGE_SIZE - page_offset)` bytes and never loops.
2. `get_writable_page()` decides ownership with `shared_ptr::use_count()`. The standard calls that value *approximate* under concurrency, so it can't safely gate an in-place write. A concurrent fork can then observe a write that happened after the fork, which breaks snapshot isolation.
3. `const_cast` on a `shared_ptr<const Page>` writes through a pointer that other holders treat as immutable. It's only correct if (2) happens to be exact.
4. `is_dirty` is copied into the child on fork, so the child inherits the parent's dirty bits. The UI's "Dirty pages: 2 private (modified by Worker #3)" would be wrong.
5. `WALFrameHeader`: `frame_id` is `uint64_t` but `parent_frame_id` is `uint32_t`, which truncates after 4 billion frames. There's no magic number or version field, so the format can't evolve. What the CRC covers is unspecified. `timestamp_us` puts wall-clock time inside the determinism boundary. `#pragma pack(1)` combined with `alignas(64)` serves no purpose, because the fields are already naturally aligned.
6. A 64 B header per token frame is about 94% overhead for ~4-byte tokens. Record one frame per completion, with an optional chunked token-timing side table.
7. Missing `<cstring>` and `<algorithm>`. There is also no endianness handling for an on-disk format.
8. `char operation_name[64]` inside every span wastes cache. Intern the strings and store a 32-bit id.

### 4. Strategy issues

- **Everything is built backend-first and the UI comes last.** That's the riskiest possible ordering for a portfolio project. v2 uses vertical slices, with something demoable on Pages from Milestone 0.
- **Breadth over depth.** Six "kernel-grade" subsystems at equal depth read as buzzword collection. v2 centres the story on **one insight** (persistent state + recorded oracles) and derives fork, seek, replay, recovery, speculation and merging from it. That's what makes it read as a design and not a feature list.
- **Prior art isn't acknowledged.** LangGraph already offers checkpoint "time travel" and Temporal already does deterministic replay. An interviewer *will* ask "how is this different from X?" See §20.4.
- **"FAANG portfolio strategy" is written in the design doc.** Keep that framing in private notes. The public README should read as an engineering project, not an application strategy.
- **The 3D node view** is decoration that costs frame budget and adds no insight. Cut it (§21).
- **Next.js** on Pages adds nothing over Vite for a static client-only SPA.
- **IndexedDB for the WAL** is slow and transactional in the wrong way. **OPFS `FileSystemSyncAccessHandle`** gives synchronous, file-like writes from a worker, which is what a log needs.
- **`MessageChannel` for WAL frames** structured-clones every message. Use shared-memory ring buffers for the data plane and `postMessage` only for commands.

---

# Part II — Architecture v2

## 1. The core idea

> **Agent-world state is an immutable, content-addressed tree. A step is a pure function `(state, event) → (state', effects)`. The only nondeterministic inputs (LLM output, tool I/O, wall-clock time, user edits) pass through the Oracle and are recorded.**

Every capability follows from that one sentence:

| Capability | Derivation | Cost |
|---|---|---|
| **fork** at step *k* | Copy root handle *k* into a new timeline | O(1) |
| **seek** to step *k* | Read `roots[k]` | O(1) |
| **Atomic steps / crash safety** | The new root is published only at commit, like shadow paging in LMDB or ZFS | No undo log |
| **Replay** | Re-run reducers, feeding them recorded oracle responses | O(steps) |
| **Divergence detection** | Compare recomputed Merkle digest with the recorded one at each commit | O(1) per step |
| **Branch diff** | Merkle descent that skips identical subtrees | O(changed) |
| **Token savings** | Oracle responses are cached by request hash, so identical requests across branches are free | — |
| **Speculative search** | Fork K branches, run them in parallel, score them, prune the losers. GC reclaims their unique nodes. | O(K) forks |
| **3-way merge** | Fork point = common ancestor | O(changed) |

The same idea shows up across systems: Git and ZFS (content-addressed CoW trees), Datomic (immutable database values), Temporal and rr (record the nondeterminism, replay the rest) and vLLM PagedAttention (ref-counted CoW blocks for KV-cache prefixes). Khonsu applies it to agent runtimes, and saying so plainly is a strength in an interview.

## 2. System overview

```text
                    ┌──────────────────────────────────────────────────────────────────┐
                    │                          KHONSU KERNEL                           │
                    │                                                                  │
   user edits ────► │  ┌────────────┐   events   ┌──────────────────┐  effects         │
   (fork, inject,   │  │ Deterministic│◄─────────│  Agent Reducers  │─────────┐        │
    kill, chaos)    │  │  Scheduler  │──────────►│ (pure: S,E → S′) │         │        │
                    │  │ (per timeline)│ step     └────────┬─────────┘         ▼        │
                    │  └──────┬──────┘                    │ reads/writes  ┌──────────┐ │
                    │         │ commit                    ▼               │  ORACLE  │ │
                    │         │               ┌──────────────────────┐    │ (only    │ │
                    │         ├──────────────►│ Persistent State Tree│    │ nondeter-│ │
                    │         │  root_k       │ HAMT + radix VFS     │    │ ministic │ │
                    │         │               │ Merkle digests       │    │ door)    │ │
                    │         ▼               └──────────┬───────────┘    └────┬─────┘ │
                    │  ┌─────────────┐                   │ node pack          │       │
                    │  │  Timeline   │  roots[], fork    │                    │record/│
                    │  │  Registry   │  parent links     ▼                    │replay │
                    │  └──────┬──────┘        ┌──────────────────────┐        │       │
                    │         └──────────────►│  Event Log (segments)│◄───────┘       │
                    │                         │  CRC32C, hash chain  │                │
                    │                         └──────────────────────┘                │
                    │  ┌───────────────────────┐   ┌────────────────────────────────┐ │
                    │  │ Sandbox: instrumented │   │ Tracer: per-thread SPSC rings, │ │
                    │  │ wasm + QuickJS, gas,  │   │ OTLP / Perfetto export         │ │
                    │  │ WASI-subset syscalls  │   └────────────────────────────────┘ │
                    │  └───────────────────────┘                                      │
                    └──────────────────────────────────────────────────────────────────┘
                         ▲ commands (postMessage)          │ data plane (SAB rings, seqlock)
                         │                                 ▼
                    ┌──────────────────────────────────────────────────────────────────┐
                    │  UI (main thread): React chrome + WebGL2 renderers, reads per rAF │
                    └──────────────────────────────────────────────────────────────────┘
```

**Threading on the web:** kernel workers (one per core, from the wasm pthread pool), a log I/O worker that owns the OPFS handle, sandbox workers (killable), and a render path on the main thread. Natively, the same kernel runs on `std::jthread`s and uses file-backed segments.

## 3. State model: persistent Merkle state tree

### 3.1 Structures

- **Maps (agent memory, blackboard, actor table, directories):** a 32-way **HAMT** (Bagwell) keyed by a 64-bit hash of the key. Nodes are bitmap-compressed.
- **Files:** a 32-way **persistent radix tree** over 4 KiB pages. Three levels address 128 MiB per file, four address 4 GiB. Page leaves are the unit of CoW.
- **Conversation histories:** a **persistent vector** (RRB tree), so forks share message-history prefixes structurally. This mirrors KV-cache prefix sharing, which §19's moonshot builds on.

### 3.2 Why handles instead of `shared_ptr`

| | `shared_ptr<Page>` (v1) | `NodeRef` (u32 arena handle) (v2) |
|---|---|---|
| Size | 16 B + separate control block | 4 B |
| Copying | Atomic RMW (contended across threads) | Plain copy |
| Reclamation | Deterministic but serialised by atomics | Epoch-based tracing GC from live roots |
| wasm32 fit | 8 B pointers + control blocks | Dense arrays that serialise cleanly |
| Persistence | Pointers need translation | Handles map directly to pack offsets |

Nodes are **immutable once published**, so readers need no locks. Each worker allocates from a thread-local bump arena, with no contention. Reclamation uses **epochs**: workers announce an epoch at each step boundary, and a mark-and-sweep from the retained roots (timeline heads, pinned steps, the retention window) runs once every worker has moved past the epoch in which a root was dropped.

### 3.3 Digests

- In-memory identity: **XXH3-128**, computed **lazily at step commit** and only for nodes created during that step. Cost is O(dirty), not O(state).
- Log hash chain and exported traces: **BLAKE3**, which makes them tamper-evident.
- A digest of all zeros means "not yet committed". A node's digest is computed at most once.

### 3.4 Sketch (C++23)

```cpp
namespace khonsu::state {

using NodeRef = std::uint32_t;                 // 0 == empty; index into a chunked arena
struct Digest128 { std::uint64_t lo, hi; auto operator<=>(const Digest128&) const = default; };

// HAMT / radix interior node. Immutable after publication.
struct Branch {
  std::uint32_t child_mask;    // bit i => slot i populated
  std::uint32_t leaf_mask;     // bit i => child is a leaf, else a Branch
  NodeRef       children;      // popcount(child_mask) contiguous NodeRefs in the arena
  std::uint32_t _pad;
  Digest128     digest;        // lazily computed at commit
};
static_assert(sizeof(Branch) == 32);

struct PageLeaf {              // 4 KiB CoW unit
  Digest128 digest;
  alignas(64) std::array<std::byte, 4096> bytes;
};

struct StateRoot {             // the whole agent world at one step
  NodeRef kv, fs, actors;      // three HAMTs
  Digest128 digest;            // H(kv.digest, fs.digest, actors.digest)
};

// Path-copying write: O(log32 n) new interior nodes + at most one 4 KiB copy.
// The old root remains valid — that *is* the snapshot.
[[nodiscard]] StateRoot write(Arena&, const StateRoot&, FilePath, std::uint64_t off,
                              std::span<const std::byte> src);   // loops across page boundaries

// Merkle diff: descends only where digests differ.
void diff(const Arena&, const StateRoot& a, const StateRoot& b, DiffSink&);

} // namespace khonsu::state
```

## 4. Agents as reducers, effects as oracle calls

This resolves blocker B2.

```cpp
namespace khonsu {

struct Event  { EventKind kind; ActorId to; Bytes payload; };            // message, oracle reply, timer, user edit
struct Effect { EffectKind kind; Bytes payload; };                        // LlmCall, ToolCall, Send, Spawn, Exit, Sleep

struct StepResult { state::StateRoot next; SmallVec<Effect, 4> effects; };

// Agent logic is a pure function of (state, event). Built-in agents are C++,
// user-defined agents are sandboxed JS/wasm invoked per step and stateless between steps.
using Reducer = StepResult (*)(const StateView&, const Event&, StateBuilder&);

} // namespace khonsu
```

- An agent's "process state" is its **HAMT subtree**: conversation history, plan, scratch memory and budgets. No stacks, coroutine frames or live wasm instances exist between steps, so **every step boundary is a valid fork point**.
- Effects are returned, not performed. The kernel carries them out, and the results come back as `Event`s. That's the Elm / Temporal-workflow split, and it's what makes replay possible.
- C++20 coroutines are still used **inside the kernel** for oracle I/O plumbing (bridging to JS promises), never for agent state. This also avoids Asyncify's code-size and runtime cost.

### 4.1 The Oracle

```cpp
class Oracle {                       // the ONLY door for nondeterminism
public:
  virtual Task<OracleReply> llm(const LlmRequest&)   = 0;
  virtual Task<OracleReply> tool(const ToolRequest&) = 0;   // network, "real world" tools
  virtual std::uint64_t     wall_clock_ms()          = 0;
  virtual std::uint64_t     entropy()                = 0;   // seeds only; PRNGs are in-kernel
};

// Live       : calls the backend, appends OracleRequest/OracleResponse frames.
// Replay     : serves the next recorded response and ASSERTS request_hash matches.
// Cached     : content-addressed by request_hash across all timelines (token savings).
// Replay→Live: on the first request_hash mismatch, the timeline goes live from there.
```

**Replay-until-divergence** is the feature to lead with in a demo. You edit the prompt at step 12 and press play. Steps 12–15 replay from cache because their requests are byte-identical. Step 16's request hash differs, and only then does Khonsu call the LLM. It works like a build system's action cache applied to agent trajectories.

## 5. Event log

The log is the **source of truth** (event sourcing). It also serves as the durability WAL.

### 5.1 Frame format

```cpp
enum class FrameType : std::uint8_t {
  Genesis, StepBegin, StepCommit, Message, OracleRequest, OracleResponse,
  ScheduleDecision, Fork, Merge, Checkpoint, ActorSpawn, ActorExit, GasExhausted, Fault,
};

struct FrameHeader {                  // 32 B, natural alignment, little-endian on disk
  std::uint32_t magic_ver;            // 'K' 'H' major minor
  std::uint32_t crc32c;               // covers bytes [8, 32 + payload_len)
  std::uint64_t lsn;                  // dense, monotonic within a timeline's log
  std::uint64_t vtime_ns;             // VIRTUAL time; wall time only via OracleResponse
  std::uint32_t payload_len;          // payload follows, zero-padded to 8 B
  FrameType     type;
  std::uint8_t  flags;                // COMPRESSED, CONTINUATION, …
  std::uint16_t actor;
};
static_assert(sizeof(FrameHeader) == 32 && std::is_trivially_copyable_v<FrameHeader>);
// Decode with std::memcpy / std::bit_cast from the byte buffer — never reinterpret_cast
// an unaligned buffer. Byteswap on big-endian hosts (std::endian check at compile time).
```

- **32 B instead of 64 B** halves the overhead for the common small frame. Payloads use a compact binary encoding (hand-rolled varint TLV, or FlatBuffers for zero-copy reads in the UI).
- **`StepCommit`** payload: `step`, `state_digest`, and `chain = BLAKE3(prev_chain ‖ frames since prev commit)`. That gives integrity per frame (CRC) and for the whole history (hash chain).
- **CRC32C** uses SSE4.2 `_mm_crc32_u64` or ARMv8 `__crc32cd` natively, and slicing-by-16 tables in wasm, since wasm SIMD has no carry-less multiply.

### 5.2 Segments and branching logs

- Each log is a sequence of 64 MiB segments with a sparse `lsn → offset` index at the tail and a torn-write-safe footer.
- **A forked timeline's log doesn't copy its parent's.** It stores `(parent_timeline, fork_lsn)` and reads below `fork_lsn` go to the parent's segments. That's the same shape as Git branches and LSM layering.
- **Group commit:** appends go into a ring, and the I/O worker flushes every *T* ms or *B* bytes. The durability point is exposed as an LSN that the UI shows ("durable up to step 4,812").

## 6. Timelines: fork, seek, replay, divergence

```cpp
struct StepIndexEntry { std::uint64_t commit_lsn; state::StateRoot root; };   // 32 B

struct Timeline {
  TimelineId id, parent;               // parent == kNone for genesis
  std::uint64_t fork_step;             // step in parent where we diverged
  std::uint64_t seed;                  // splitmix64(parent.seed ^ id): per-branch PRNG streams
  LogHandle log;                       // own segments only
  std::vector<StepIndexEntry> steps;   // steps[k] -> root  ⇒ O(1) seek
  OracleMode mode;                     // Replay | Live | ReplayThenLive
};

TimelineId Kernel::fork(TimelineId src, std::uint64_t k) {        // O(1)
  auto& p = timelines_[src];
  auto& t = timelines_.emplace(next_id_++, p.id, k, derive_seed(p.seed, next_id_));
  t.steps.push_back(p.steps[k]);       // share root: zero nodes, zero pages copied
  t.log.append(frames::Fork{.parent = src, .step = k});
  gc_.pin(p.steps[k].root);            // fork points are always retained
  return t.id;
}
```

### 6.1 Retention policy

By default every step keeps its root (32 B of index per step; the state itself is shared structurally). For long runs the older history is thinned **log-spaced**: every step within the last *W*, then every 2nd, 4th, 8th and so on further back, the way backup rotation works. Pinned roots (fork points, bookmarks, merge bases) always survive. Seeking to a thinned step means taking the nearest retained root and replaying at most *gap* frames, which is still bounded.

### 6.2 Replay and divergence detection

At each `StepCommit` during replay, the recomputed `state_digest` is compared with the recorded one. On a mismatch Khonsu:

1. halts at the **first divergent step**,
2. runs a Merkle diff between the expected and actual state and shows the exact keys and pages that differ,
3. reports the causal frame that produced them.

This is a determinism checker like FoundationDB's, and in a demo it's worth more than any feature on the v1 list.

## 7. Deterministic scheduler and concurrency model

### 7.1 Principle: parallel *across* timelines, deterministic *within* one

- **Within a timeline**, one logical thread runs at a time. Ready events sit in a priority queue ordered by `(vtime, priority, dest_actor, sender_actor, seq)`, which is a total, reproducible order. Virtual time advances by recorded oracle latencies, so a replay also *looks* identical on the flame graph.
- **Across timelines**, a work-stealing pool (Chase–Lev deques) runs independent timelines in parallel on every core. Speculative search (§7.3) scales linearly here, because timelines share only immutable nodes.
- **Live-mode nondeterminism:** when several live oracle replies race, arrival order would decide which actor runs next. The scheduler consumes replies only at step boundaries and **logs a `ScheduleDecision` frame** only when a choice depended on arrival order. Replay follows the log. This is rr's approach, applied at the level of agent steps.

### 7.2 Banned inside the determinism boundary (enforced by clang-tidy checks and review)

- Iterating `std::unordered_*` containers: libc++ and libstdc++ order differs. Use sorted vectors or HAMT order.
- `std::random_device`, `rand()`, `time()`, `chrono::system_clock` and randomized hash seeds. Use seeded xoshiro256\*\*.
- libm transcendentals (`sin`, `exp`, `pow`): results differ between glibc, musl and Emscripten. Vendor one correctly rounded implementation, or keep floating point out of reducers.
- `-ffast-math` and FP contraction (`-ffp-contract=off`). Wasm **Relaxed SIMD** is nondeterministic across hardware, so it's allowed only outside the boundary (for example in the renderer). NaN payloads are canonicalised at the sandbox boundary.
- Pointer values as keys or for ordering.

**CI check:** the native (x86-64, arm64) and wasm builds replay the same traces and must produce **bit-identical state digests**.

### 7.3 Speculative execution

At a decision point, `fork` produces K branches (varied prompts, temperatures, tools). They run in parallel, an evaluator scores each (tests passing inside the sandbox, or an LLM judge), and the winner is **promoted** to head while the losers are pruned, freeing their unique nodes. This is beam search / MCTS over agent trajectories. It's the practical reason cheap forking matters, and it gives the demo its best visual: the multiverse fanning out and collapsing.

### 7.4 Simulated cluster (replaces "distributed actor mesh")

Actors can be placed on **simulated nodes** connected by a simulated network with configurable latency, loss, duplication and partitions, all driven by the timeline's seeded PRNG. Chaos buttons ("partition node 2", "kill planner", "drop 20% of messages") inject **recorded fault events**, so every chaos run can be replayed exactly. A supervisor applies Erlang-style restart strategies (one-for-one, rest-for-one) and restarts actors from their last committed state. The native build can optionally run real multi-process nodes over TCP, but the portfolio claim is simulation, stated honestly.

## 8. Sandbox and gas metering

Agent-generated code (tests, scripts, tools) runs in a sandbox that is both **isolated** and **replayable**.

| Layer | Mechanism |
|---|---|
| **Runtime** | **QuickJS-ng compiled to wasm** runs agent-generated JavaScript. `Date` and `Math.random` are replaced with kernel-provided deterministic versions. Arbitrary user wasm modules are also accepted. Python via Pyodide is a lazy-loaded stretch goal. |
| **Gas** | A C++ **wasm binary rewriting pass** (written in-house, used in both targets) computes per-basic-block static costs. It inserts `gas -= cost` at block entry and an `if (gas < 0) trap` check **only at loop headers and function entries**, which is enough to bound execution because straight-line code is finite. Target overhead is ≤ 15%. QuickJS's interrupt handler adds a second, JS-level budget. |
| **Memory** | The imported memory has a fixed `maximum`. `memory.grow` failures are handled deterministically. |
| **Syscalls** | A WASI-preview1 subset. Every import goes through a **syscall table in the kernel**. File I/O reads and writes the **timeline's persistent VFS** (CoW for free). Network and clock calls go through the **Oracle**, so they're recorded. Anything else traps. |
| **Backstop** | Each sandbox runs in its own worker that the kernel can `terminate()` on a wall-clock watchdog. Natively, the same instrumented module runs on Wasmtime or WAMR, with Wasmtime's epoch interruption as the backstop. |

Because file writes land in the persistent VFS, a sandbox run that crashes or runs out of gas leaves **no partial side effects**: its writes were never committed to a root. That directly addresses v1's "state pollution on failure" problem.

## 9. Merging timelines (and where CRDTs actually belong)

- **Timeline merge is a 3-way merge.** The fork point is the common ancestor. A Merkle diff of `(base, ours)` against `(base, theirs)` finds the changed keys and pages in O(changed). Disjoint changes merge automatically. Overlapping text files use diff3 line merging, and anything else is shown as a conflict in the UI. The result is a timeline with **two parents**, like a Git merge commit.
- **CRDTs are for keys declared mergeable.** State cells can carry a type annotation, which gives conflict-free merges where the semantics are clear:
  - `PNCounter`: token and gas budgets spent across branches
  - `ORSet` (observed-remove; unlike LWW-Set it doesn't lose concurrent adds): discovered facts, TODO sets
  - `LWWRegister` ordered by `(vtime, timeline_id)`: deterministic, and with no physical-clock skew
  - Sequence CRDT (Fugue or RGA): a shared scratchpad that several agents edit concurrently *within* one timeline
- Property tests check the CRDT laws (commutativity, associativity, idempotence) (§17).

## 10. Durability and crash recovery

- **Log:** OPFS segments on the web, files natively (`O_DIRECT`/`fdatasync` optional). Group commit, with a CRC on every frame. Recovery scans from the last good footer and truncates at the first bad CRC.
- **Node packs:** at each checkpoint, nodes that are **reachable and not yet persisted** are appended to a pack file, deduplicated by digest, the way Git packfiles work. Checkpoints are therefore incremental, costing O(new nodes).
- **Recovery:** load the pack index, take the latest checkpointed root, and replay the log tail in replay mode. If the recovered digest matches the recorded chain, recovery is verified.
- **Actor crash:** the supervisor restarts the actor from its last committed subtree. Since steps are atomic, there's never a half-applied state to clean up.

## 11. Tracing and observability

- Spans (`trace_id`, `span_id`, `parent`, `start`, `end`, `gas`, `bytes_allocated`, `name_id`) are 64 B records written into **per-thread SPSC ring buffers**. There's no allocation or locking on the hot path, and names are interned.
- Timestamps use virtual time for replayable views and `rdtsc` / `performance.now()` for real profiling. With cross-origin isolation `performance.now()` has about 5 µs resolution, and about 100 µs without it.
- **Causal links:** a fork creates an OTel *span link* from the child's root span to the parent's step span, so traces follow the timeline DAG.
- **Export:** OTLP/JSON (loads in Jaeger or Grafana Tempo) and Chrome Trace Event / Perfetto format. There's a one-click "Open in Perfetto" (ui.perfetto.dev accepts traces via `postMessage`).

---

# Part III — Platform, Frontend, Performance

## 12. Running on GitHub Pages

| Constraint | Solution |
|---|---|
| No custom headers, so no COOP/COEP, so no `SharedArrayBuffer` | Register the **`coi-serviceworker`** pattern: a service worker that re-serves every response with `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` (or `credentialless`). The first visit reloads once. |
| The service worker can be unavailable (Firefox private mode, some embedded browsers) | **Two builds**, `khonsu-mt.wasm` (pthreads + SIMD) and `khonsu-st.wasm` (SIMD, no threads), chosen at boot via `crossOriginIsolated`. The UI shows a small "single-threaded mode" badge. Everything works in both, and only speculative-search throughput changes. |
| COEP blocks cross-origin assets | Host all assets on the same origin. Fetch model weights from Hugging Face, which sends CORS headers and works under `credentialless`. |
| Emscripten flags | `-pthread` (not the deprecated `-sUSE_PTHREADS`), `-msimd128`, `-O3 -flto`, `-fwasm-exceptions` (or `-fno-exceptions` in the kernel), `-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency`, `wasm-opt -O4`. Size budgets are enforced in CI. |
| Persistence | **OPFS** via `createSyncAccessHandle()` in the log worker, which is only available in dedicated workers and is exactly the right API for an append-only log. |
| Caching (Pages sends `max-age=600`) | Content-hashed filenames, and the service worker precaches the wasm and scenario traces, so repeat visits load instantly and work offline. |
| SPA routing under `/<repo>/` | Vite `base: '/Khonsu/'` plus hash routing, or the `404.html` redirect trick. |
| Size limits (1 GB site, 100 MB per file) | Scenario traces are gzip-compressed `.khn` files decoded with the built-in `DecompressionStream`. Model weights are never committed. |
| Deep links | Put state in the URL hash, e.g. `#/s/hallucination?t=B&step=12&panel=diff`. An interviewer can open the exact moment you're describing. |

## 13. LLMs on a static site

There's no server and no hidden API key. The Oracle modes make this workable:

| Mode | How | Default? |
|---|---|---|
| **Replay** | Bundled recorded traces. Costs nothing, is instant, and proves determinism. | ✅ Default for every scenario |
| **Local** | WebGPU inference via **WebLLM** with a small instruct model, downloaded on demand and cached. | Opt-in ("Go live locally") |
| **BYOK** | The user pastes a provider API key. Calls go directly from the browser to a CORS-enabled provider API. The key lives only in memory (optionally `sessionStorage`) and is never persisted or sent anywhere else. | Opt-in |
| **Scripted** | A deterministic mock "LLM" (rule-based) for tests and chaos scenarios. | Tests and CI |

Replay-then-live (§4.1) means editing a prompt in a recorded scenario only costs tokens from the point of divergence onward.

## 14. Frontend architecture

- **Stack:** Vite, React 19, TypeScript (strict), Zustand for UI state, and Tailwind with CSS variables for the design tokens.
- **The rule that keeps it at 60 fps:** React renders the *chrome* (panels, inspectors, forms). Anything with more than a few hundred elements is drawn by **custom instanced WebGL2 renderers** (WebGPU where available). Text uses an MSDF font atlas. The main thread's budget is under 4 ms per frame.
- **Data plane:** kernel workers write compact records into **SharedArrayBuffer rings** (SPSC per worker to the UI) and a **seqlock-protected metrics block**. The UI drains them once per `requestAnimationFrame`, with no `postMessage` per event. Typed-array views over wasm memory are uploaded straight into GPU buffers, avoiding intermediate JS objects. In single-threaded mode the kernel instead sends transferable `ArrayBuffer` batches once per frame.
- **Control plane:** commands (`fork`, `seek`, `inject`, `kill`) go over `postMessage` with a small typed RPC layer (Comlink or hand-rolled).
- **Views:**
  1. **Timeline Multiverse** (hero view, WebGL): x is the step and branches fan out vertically, drawn as smooth Bézier forks. Node colour encodes the state digest, so **identical states across branches show the same colour**, making convergence visible. Minimap and semantic zoom (LOD aggregation above 10k nodes).
  2. **Scrubber / transport:** play, pause, step, and seek at 60 fps across 100k+ steps, because seek is O(1).
  3. **Actor topology** (React Flow is fine here, since there are fewer than about 50 nodes): actors, mailboxes, supervisor tree, and simulated nodes with partition overlays.
  4. **CoW page heatmap:** a grid of 4 KiB pages for the selected file, coloured by *shared with parent*, *private to this branch* and *identical to branch X*. It's the most direct visualisation of CoW anywhere in the project.
  5. **Branch diff:** a Merkle-diff tree with a side-by-side text diff (Monaco diff editor, lazy-loaded).
  6. **Flame graph and timeline** (WebGL): spans coloured by actor, gas and bytes as overlays, and "Open in Perfetto".
  7. **Oracle inspector:** each LLM call's request, response, request hash, cache hit or miss, and tokens saved.
  8. **Performance HUD** (always visible): fork latency p50/p99, frames/s, replay rate, heap, *% of pages shared*, and threads active.

## 15. Visual design

**Concept: "the lunar observatory".** Khonsu is the moon god and the god of time, so the UI should feel like an instrument for observing time: dark, precise, luminous where it matters.

- **Palette:** near-black blue background (`#07090F`) with layered surfaces (`#0D111A`, `#141A26`), moon-silver text (`#D8DEE9` primary, `#8A94A7` secondary), and one warm accent (lunar amber `#F5C26B`) for the active head and primary actions. Branch hues come from an **OKLCH** palette with evenly spaced hues and fixed lightness and chroma, which gives perceptual uniformity and survives colour-blind checks. The whole palette is also defined in a light theme.
- **Type:** Inter (UI) and JetBrains Mono (hashes, LSNs, code), with tabular numerals everywhere numbers change.
- **Motion:** purposeful only. A fork animates as a branch peeling off the parent over about 250 ms with a spring. Replay shows a travelling "light" along the active path. Pruned branches fade and dissolve. `prefers-reduced-motion` is fully respected.
- **Landing page (scroll-told):** four short sections, each with a **live widget driven by the real wasm kernel**, not a video: *(1) state is a tree → (2) fork is a pointer copy → (3) writes copy only a path → (4) replay proves determinism*. The page ends with **"Launch the kernel"**, which opens the full workbench on the hallucination scenario.
- **Keyboard-first:** `space` play/pause, `J/K` step, `[` `]` jump between forks, `F` fork here, `D` diff with parent, `?` shortcut sheet.
- **Layout:**

```text
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ ☾ KHONSU   Scenario: The Step-12 Hallucination ▾      ● MT · 16 threads   fork p50 0.8µs│
├──────────────────────────────────────────────────────────────┬───────────────────────┤
│  TIMELINE MULTIVERSE (WebGL)                                 │  INSPECTOR            │
│   main ●━━━━━━━━━━━●━━━━━━━━━━━━━━━━━━━━━━━━━✕ step 19 tests │  Step 12 · Timeline B │
│                    ┗━━━━ B ●━━━━━━━━━━━━━━━━━━━━━━━━━━━✓      │  state 9f3a…c1         │
│                    ┗━━━━ C ●━━━━━━━━━━━━━━━━✓ (pruned ░)      │  oracle: cache HIT     │
│                                                              │  tokens saved: 14,210  │
│  ◀◀  ▶  ▮▮  ▶▶   ━━━━━━━━━━━━━━━━●━━━━━━━━━━━━━  12 / 45     │  [Fork] [Inject] [Diff]│
├──────────────────────────┬───────────────────────────────────┴───────────────────────┤
│  CoW PAGES /src/api.ts   │  FLAME GRAPH                                              │
│  ■■■■■■■□□■■■■■■■■■■■■■  │  ▇▇▇▇▇▇▇▇ planner ▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇     │
│  ■ shared  □ private     │    ▇▇ vfs.write ▇  ▇▇▇▇ sandbox.run(tests) ▇▇▇▇▇▇▇▇        │
└──────────────────────────┴───────────────────────────────────────────────────────────┘
```

### 15.1 Showcase scenarios (bundled traces)

1. **The Step-12 Hallucination.** A planner, coder, tester and reviewer fix a bug in a small TypeScript repo held in the VFS. At step 12 the coder invents a non-existent API, and at step 19 the tests (run inside the sandbox) fail. You scrub back, fork at 12, inject the real API doc and press play: steps 12–15 replay from cache, 16 onward runs live, the tests pass, and the diff view shows exactly what changed.
2. **Speculative search.** Sixteen branches at the planning step run across all cores, get scored by the sandboxed test suite, and the winner is promoted. The HUD shows throughput scaling with thread count.
3. **Chaos and recovery.** Partition a simulated node, kill the planner, and feed an infinite loop to the sandbox (gas runs out and nothing leaks into state). The supervisor recovers, and replaying the chaos run reproduces it bit-for-bit.
4. **Determinism proof.** Replay a 1M-step synthetic trace at full speed with digests verified at every commit. Flip one byte in the log and the CRC catches it. Tamper with an oracle response and the divergence detector points to the first bad step and the exact key that differs.

## 16. Performance targets and methodology

These are **targets to validate**. Publish the measured numbers, whatever they turn out to be.

| Metric | Native target | Web (MT) target | Why it's achievable |
|---|---|---|---|
| `fork()` | < 1 µs | < 5 µs | One descriptor allocation and one handle copy |
| First write to a shared page after fork | < 2 µs | < 10 µs | Path copy of ~3–4 interior nodes plus one 4 KiB memcpy |
| Seek to any retained step | O(1), < 1 µs | < 1 ms incl. view update | Index lookup |
| Log append (in memory, CRC32C, 64 B payload) | ≥ 5 M frames/s/core | ≥ 1 M frames/s | Hardware CRC natively, slicing-by-16 in wasm |
| Verified replay (scripted agents) | ≥ 2 M steps/s | ≥ 500 k steps/s | Digest only for dirty nodes |
| Gas instrumentation overhead | ≤ 15% | ≤ 15% | Checks only at loop headers and calls |
| Speculative search scaling | ≥ 0.8 × cores | ≥ 0.7 × cores | Timelines share only immutable data |
| UI | — | 60 fps with 100k timeline nodes and 1M spans | Instanced WebGL, LOD, rAF-drained rings |
| Cold load | — | ≤ 2.5 MB compressed, interactive < 2 s | Split wasm, lazy Monaco and WebLLM |

**Methodology:**
- Native benchmarks use Google Benchmark with pinned cores, `perf stat` counters (cache misses, IPC), and p50/p99/p999 from HdrHistogram.
- An **in-browser benchmark page** (`/bench`) runs the same suites in wasm on the visitor's own machine. It's a strong interview moment: "run it on your laptop right now."
- **Baselines run next to v2:** deep copy, v1's `shared_ptr`-per-page design, and the persistent tree, with fork cost vs state size plotted on log-log axes. The v1 design is a useful baseline, and showing that you measured it first strengthens the story.
- CI runs benchmarks on every commit to `main`, writes JSON to the Pages site, and charts regressions over time.

---

# Part IV — Engineering, Roadmap, Interview Kit

## 17. Correctness strategy

| Technique | Target |
|---|---|
| Unit tests (GoogleTest or Catch2) | All modules |
| **Model-based property tests** (RapidCheck) | Persistent HAMT, radix VFS and vector against `std::map` / `std::vector` models under random operation sequences, including fork and old-root validity |
| **CRDT law tests** | Commutativity, associativity and idempotence of `merge` across random states |
| **Fuzzing** (libFuzzer + ASan/UBSan) | Log frame decoder, segment recovery, wasm binary parser and instrumentation pass, `.khn` importer |
| **Deterministic simulation testing** | A seed-driven harness that runs scenarios with random faults (kills, partitions, torn writes, CRC flips, oracle latency shuffles) and asserts invariants: replay digest equality, no committed partial steps, and a verified hash chain after recovery. Any failure prints its seed, and `khonsu sim --seed=…` reproduces it. |
| **Cross-target determinism** | The same traces on x86-64, arm64 and wasm must give bit-identical digests |
| Sanitizers | ASan, UBSan and TSan builds in CI. TSan covers the work-stealing pool, rings and epoch GC. |
| E2E | Playwright against the real built site: COI boot, the fallback path, each scenario's golden digests, and visual snapshots |
| Static analysis | clang-tidy with custom determinism checks (§7.2) and `-Wall -Wextra -Werror` |

## 18. Repository layout, build and CI

```text
Khonsu/
├── kernel/                     # portable C++23 core, no platform dependencies
│   ├── include/khonsu/{state,log,sched,oracle,sandbox,trace,merge,crdt}/
│   └── src/…
├── platform/
│   ├── native/                 # CLI (`khonsu run|replay|sim|bench|export`), file segments, Wasmtime
│   └── web/                    # Emscripten entry, C ABI for hot paths, OPFS backend, JS oracle bridge
├── web/                        # Vite + React + TS app, WebGL renderers, landing page
├── scenarios/                  # scenario definitions + recorded .khn traces + golden digests
├── tests/  fuzz/  bench/  sim/
├── tools/                      # trace inspector, .khn ⇄ OTLP/Perfetto converters
├── docs/                       # DESIGN.md, ADRs (docs/adr/NNN-*.md), benchmark reports
├── CMakePresets.json           # native-debug, native-asan, native-tsan, native-release, wasm-mt, wasm-st
└── .github/workflows/          # ci.yml, bench.yml, pages.yml
```

- **Build:** CMake presets and CPM or vcpkg for dependencies (xxhash, BLAKE3, QuickJS-ng, GoogleTest, RapidCheck, Google Benchmark). The Emscripten version is pinned.
- **Bindings:** a flat C ABI plus typed-array views for hot paths. embind is used only for cold control calls, because of its per-call overhead.
- **ADRs:** record each major decision (handles vs `shared_ptr`, reducers vs coroutines, 3-way merge vs CRDT, 32 B frames) as a short ADR. Walking an interviewer through ADRs demonstrates engineering judgement directly.
- **CI:**
  - `ci.yml`: the native matrix (clang/gcc × linux/macos, plus sanitizer builds), both wasm builds with size budgets, unit, property and fuzz-smoke tests, 10k DST seeds, and Playwright.
  - `bench.yml`: runs benchmarks and publishes JSON.
  - `pages.yml`: builds the web app and deploys it with `actions/deploy-pages`.

## 19. Roadmap (vertical slices)

Every milestone ends with something **live on Pages** that you could demo in an interview.

| Milestone | Scope | Demo at the end |
|---|---|---|
| **M0: Walking skeleton** | Persistent HAMT, in-memory log, scripted agents, reducer loop, wasm-st build, scrubber UI, COI service worker, Pages deploy | Scrub a recorded run in the browser |
| **M1: Fork and the multiverse** | O(1) fork, radix VFS, Merkle diff, WebGL multiverse, CoW page heatmap, benchmarks against deep copy and v1 | Fork at step 12, watch pages turn private |
| **M2: Determinism and durability** | Frame format, CRC32C, hash chain, OPFS segments, node packs, recovery, divergence detector, DST harness, cross-target CI | Scenario 4 (determinism proof) |
| **M3: Sandbox** | Wasm instrumentation pass, QuickJS-ng, syscall table over the VFS, gas UI, watchdog | An infinite loop is contained and no state leaks |
| **M4: Parallelism and tracing** | wasm-mt build, work-stealing pool, SAB data plane, epoch GC, tracer, flame graph, Perfetto export | Scenario 2 (speculative search scaling) |
| **M5: Live oracles and merge** | Oracle cache, replay-then-live, WebLLM and BYOK, 3-way merge, CRDT cells, simulated cluster and chaos | Scenario 1 end-to-end live, then Scenario 3 |
| **M6: Polish** | Landing page, HUD, deep links, accessibility, light theme, README GIFs, `/bench` page, ADRs | The public launch |
| **Moonshot** | **Forkable KV cache:** own WebGPU inference for a small model with a paged KV cache managed by the same CoW allocator (PagedAttention-style block tables), so `fork()` also forks the model's attention state and speculative branches reuse prefix KV blocks | "Fork shares the GPU memory too" |

## 20. Interview kit

### 20.1 30-second pitch

> "Agent frameworks treat state as mutable blobs and LLM calls as fire-and-forget, so you can't reproduce, branch or safely roll back a run. Khonsu treats agent state as an immutable Merkle tree and routes every nondeterministic input through one recorded interface. That makes fork a pointer copy, seek O(1), replay bit-exact with automatic divergence detection, and parallel speculative execution cheap. It's a C++ engine that runs natively and entirely in your browser via WebAssembly. Here's the live link."

### 20.2 Whiteboard order (about 5 minutes)

1. Draw the state tree, fork as copying a root, and a write as a copied path. This is the core idea.
2. The reducer / effect / oracle split, and why it makes replay possible.
3. The log format and branching logs. Replay-until-divergence.
4. Concurrency: deterministic within a timeline, parallel across timelines.
5. Numbers from your benchmarks: fork latency, scaling curve, fork cost vs state size compared with the baselines.

### 20.3 Questions to expect, with short answers

| Question | Answer |
|---|---|
| *Why not `shared_ptr` per page?* | Fork becomes O(pages) atomic increments, each page costs 16 B, and `use_count` isn't a safe ownership test across threads. I measured it: see the chart. |
| *How do you reclaim memory?* | Epoch-based tracing GC from retained roots. Nodes are immutable, so readers never block. The retention policy thins old history log-spaced. |
| *What if an LLM is nondeterministic even at temperature 0?* | It doesn't matter. Replay never re-asks the model; it serves recorded responses keyed by request hash. |
| *How do you know replay is deterministic?* | A digest at every commit, cross-target CI (x86, ARM and wasm must agree), and DST with thousands of seeds. |
| *What breaks determinism in C++ specifically?* | Unordered iteration, libm differences, FP contraction, fast-math, Relaxed SIMD, NaN payloads, pointer ordering, wall clocks. They're banned by lint inside the boundary. |
| *How does this scale past one machine?* | Timelines are independent units, so they shard naturally by timeline. The content-addressed node store is a CAS and can be backed by S3-like storage. The log is per timeline, so there's no global ordering. Cross-node merges use the same 3-way/CRDT logic. |
| *Crash in the middle of a step?* | The new root is never published, so the state is still the previous root. It's shadow paging, so no undo log is needed. |
| *Why a wasm sandbox instead of containers?* | Startup in microseconds rather than milliseconds, deterministic execution, it runs in the browser, and syscalls are routed through my VFS so side effects become forkable. |
| *Hardest bug?* | Keep a real war story from the build (for example a TSan race in epoch GC, or a cross-target digest mismatch traced to `std::unordered_map` order). |

### 20.4 Prior art and positioning

| System | What Khonsu borrows | What Khonsu adds |
|---|---|---|
| **LangGraph** checkpoints / time travel | Step-level checkpoints | Structural sharing (O(1) fork, not a serialised copy per step), deterministic scheduling of *concurrent* agents, a sandboxed CoW filesystem for side effects, divergence detection |
| **Temporal** | Deterministic workflow replay from an event history | Forking and branching, in-memory O(1) seek, speculative parallel branches |
| **rr** | Record nondeterministic inputs and scheduling decisions | Works at agent-step granularity, with branchable replays |
| **FoundationDB / TigerBeetle simulation** | Deterministic simulation testing | Applied to agent runtimes, visualised |
| **Git / ZFS / LMDB / Datomic** | Content-addressed CoW trees, shadow paging, database-as-a-value | A live execution runtime, not just storage |
| **vLLM PagedAttention** | Ref-counted CoW blocks | The same allocator applied to agent state (and KV cache in the moonshot) |
| **Wasmtime fuel / epochs** | Gas and interruption | A portable instrumentation pass that also works in browsers |

## 21. Non-goals and cut list

- **Cut:** the 3D node view, Next.js, IndexedDB for the log, and `MessageChannel` as the data plane.
- **Renamed or reframed:** "distributed actor mesh" becomes a *deterministic simulated cluster*; "CRDT timeline merge" becomes *3-way merge plus typed CRDT cells*; "WAL" becomes an *event log that also serves as the WAL*.
- **Non-goals:**
  - Forking arbitrary running native processes. Khonsu forks at step boundaries by design.
  - Being a general-purpose agent framework. The built-in agents exist to demonstrate the runtime.
  - Security against a malicious *host*. The sandbox protects the host from agent code, not the other way round.
