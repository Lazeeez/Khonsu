# ☾ Khonsu

**A deterministic, forkable runtime for multi-agent LLM systems.** C++23 kernel compiled to WebAssembly, with an interactive workbench that runs entirely in your browser.

Agent-world state (files, agent memory, *and the scheduler's queue*) is one immutable, content-addressed Merkle tree. Every source of nondeterminism (LLM output, tool I/O, wall-clock time) enters through a single recorded door, the **Oracle**. Everything else follows from those two decisions:

| | How | Cost (measured, wasm32) |
|---|---|---|
| **fork()** at any step | copy one 4-byte root handle | **~1 ns** store-level · **~0.7 µs** full kernel fork |
| **seek** to any step | every committed step keeps its root | O(1) |
| **Copy-on-write** | path-copying HAMT; 4 KiB pages shared until written | ~1–2 µs first write per page |
| **Bit-exact replay** | re-run reducers against recorded oracle responses | ~6–40 µs / step, digest-checked |
| **Divergence detection** | Merkle digest compare at every commit + hash chain over frame CRCs | first bad step + key-level diff |
| **Replay-then-live** | oracle cache keyed by request hash: unchanged prompts are free | tokens saved shown live |
| **Speculative search** | fork N sampled branches, run, score, prune; GC reclaims losers | O(N) forks |

At 64 MiB of state, fork is **~110,000× faster than the v1 `shared_ptr`-per-page design** and **~6,000,000× faster than a deep copy** (see the **Benchmarks** tab of the workbench, or run `node build/wasm/khonsu_bench.js`).

## The demo

The workbench replays a recorded "incident": a planner, researcher, coder, tester and reviewer try to fix issue #42 in a small TypeScript repo. The planner skips the docs and the coder **hallucinates `api.getUserById()`** three times. The guided tour then walks through:

1. **Replay** the recorded run step by step. The multiverse, CoW page map and causal trace all scrub together.
2. **Root cause**: jump to the hallucinated call and read the recorded LLM request and response frames.
3. **Fork & fix**: fork at genesis and inject the real API into the coder's memory. The planner's unchanged prompt **replays from the recording** and only the coder goes live. The generated client file shares 11 of 12 pages with the original.
4. **Speculate ×8**: eight differently-sampled branches; the evaluator (the sandboxed test suite) keeps the best.
5. **Trust, but verify**: replay from the log alone, then tamper with a recorded response *and fix its CRC*. The verifier still pinpoints the step.
6. **Chaos**: kill the coder mid-run. The supervisor restarts it from committed state, and the run still verifies.

> The LLM in the offline demo is a **simulated model** (`khonsu-sim-1`) whose sampling draws on host entropy, exactly the kind of nondeterminism the Oracle has to record. Real backends (WebLLM / BYOK APIs) plug in behind the same `LanguageModel` interface; see the roadmap.

## Architecture in one diagram

```text
 user edit / chaos ─► Deterministic scheduler ──► Agent reducer (pure: state, event → state′, effects)
                       (queue lives IN the state)            │ effects
                              │ commit root_k                ▼
                              ▼                        ORACLE (record │ replay │ cache)
                  Persistent Merkle state tree ◄──────────────┘
                  32-way HAMT · 4 KiB CoW pages · lazy XXH3-128 digests · canonical form
                              │
                              ▼
                  Event log: 32 B frames · CRC32C · StepCommit(digest, hash chain) · .khn export
```

The full rationale, including the review of the original proposal, trade-offs, prior art and the roadmap, is in **[docs/DESIGN.md](docs/DESIGN.md)**.

## Repository layout

```text
kernel/          portable C++23 core: store (HAMT + CoW files + Merkle), log, kernel, model, bench
platform/web/    flat C ABI for the WebAssembly module (JSON on cold paths, exception-safe)
web/             Vite + React + TypeScript workbench; kernel runs in a Web Worker; canvas renderers
tests/           unit + model-based property tests (dependency-free harness)
bench/           benchmark CLI (same suite as the in-browser /bench page)
third_party/     xxHash (BSD-2)
```

## Build

**WebAssembly (primary target).** Requires [emsdk](https://emscripten.org), CMake ≥ 3.24 and Ninja.

```bash
cmake --preset wasm && cmake --build --preset wasm   # also copies khonsu.{mjs,wasm} into web/public/wasm
ctest --preset wasm                                  # tests run under Node
node build/wasm/khonsu_bench.js                      # benchmarks (JSON)

cd web && npm ci && npm run dev                      # http://localhost:5173/Khonsu/
npm run build && npx vite preview --port 4173 &
node e2e/smoke.mjs http://localhost:4173/Khonsu/     # headless E2E of the whole tour
```

**Native** (Linux/macOS, clang or gcc): `cmake --preset native && cmake --build --preset native && ctest --preset native`. The `native-asan` preset builds with ASan and UBSan.

**Deploy.** `.github/workflows/pages.yml` builds native and wasm, runs the tests, sanitizers and a Playwright E2E, then publishes `web/dist` to GitHub Pages. Enable Pages with source "GitHub Actions" in the repo settings.

## Correctness

- **Model-based property tests**: 4,000 random put/erase/file ops against a `std::map` model, with *every historical root* re-checked afterwards (persistence).
- **Canonical structure**: equal key sets give equal Merkle roots regardless of edit history, which is what makes "same colour = same world" in the UI true.
- **Determinism across entropy**: 40 different host-entropy seeds; every run verifies, and imported logs replay bit-exactly in a kernel with *different* entropy.
- **Adversarial**: bit-rot is caught by CRC32C; a CRC-fixed tamper is caught by the replay digest and the hash chain.
- **A real war story**: `verify()` diverged only at `-O2`, and only on the *second* call. The cause was xxHash's streaming `reset_withSeed` on a stack-emplaced state: it skips deriving the secret when the uninitialized `seed` field happens to match, so hashes depended on stale stack bytes. Replay verification turned a silent heisenbug into a reproducible failure (fixed in `kernel/src/common.cpp`).

## License

MIT. See [LICENSE](LICENSE).
