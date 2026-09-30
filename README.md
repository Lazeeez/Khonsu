# ☾ Khonsu

**A time-travel debugger for AI agents.** Record any agent run once, including every model call, every tool call and the files it touched. Then replay it bit-exactly, with no model, no tokens and no network, as many times as you like. Jump to any call and see the workspace as it was at that moment. Fork a new future from there and pay only for what changed. When a replay drifts, Khonsu names the exact JSON field or file that differs.

Named after the Egyptian moon god whose name means *traveller*: Khonsu takes you back to the moment before things went wrong.

[![PyPI](https://img.shields.io/pypi/v/khonsu-sdk?label=PyPI%20khonsu-sdk)](https://pypi.org/project/khonsu-sdk/)
[![npm](https://img.shields.io/npm/v/khonsu-sdk?label=npm%20khonsu-sdk)](https://www.npmjs.com/package/khonsu-sdk)

**[Website](https://lazeeez.github.io/Khonsu/)** · **[Guide](https://lazeeez.github.io/Khonsu/#/guide)** · **[Download](https://lazeeez.github.io/Khonsu/#/downloads)** · **[Open a recorded run](https://lazeeez.github.io/Khonsu/#/viewer?src=support-agent.khn)** · **[Fork a running program](https://lazeeez.github.io/Khonsu/#/play)** · **[Results](docs/RESULTS.md)**

---

## Contents

[The problem](#the-problem) · [Get Khonsu](#get-khonsu) · [A tour in 60 seconds](#a-tour-in-60-seconds) · [Highlights](#highlights) · [Tools and SDKs](#tools-and-sdks) · [Deterministic tests in CI](#deterministic-agent-tests-in-ci) · [Share runs](#share-runs-with-your-team) · [The website](#the-website) · [Results](#results) · [Architecture](#architecture) · [Tech stack](#tech-stack) · [Repository layout](#repository-layout) · [Correctness and quality](#correctness-and-quality) · [CI, releases and distribution](#ci-releases-and-distribution) · [Roadmap](#roadmap) · [Documentation](#documentation)

## The problem

AI agents are hard to debug because they never do the same thing twice.

| Problem teams have with agents | What Khonsu does |
|---|---|
| **Failures can't be reproduced.** The model samples differently on every run, so the bug disappears when you try again. | `khonsu record` captures every exchange durably, even through `kill -9`. `khonsu replay` reproduces the run byte-for-byte, offline. |
| **Agent tests are flaky and expensive.** Every CI run calls the model again. | `@pytest.mark.khonsu("run.khn")`: the test replays a committed recording, deterministic, offline and free. A behaviour change fails with a field-level report. |
| **Prompt iteration burns tokens.** Editing step 15 re-pays for steps 1–14. | `--on-miss live` or `khonsu fork run.khn@15`: unchanged calls come from the recording, and only the changed ones reach the model. |
| **Agents have side effects.** Re-running one re-sends the email and re-charges the card. | `@khonsu.tool` records tool results. Replay returns them without running the side effect again, and every call snapshots the workspace. |

Existing tools cover pieces. Tracing products (LangSmith, Langfuse, Phoenix) *show* runs but can't *re-execute* them. HTTP cassettes (vcrpy) break on any prompt change. Framework checkpointers (LangGraph) are tied to one framework and don't capture the workspace. Khonsu works with any language and any framework that talks to an OpenAI-compatible or Anthropic API, runs locally, and is free.

In a study on a real coding agent, the same task with the same prompt produced different code in **16 of 16** live re-runs and flipped between pass and fail on **5 of 8** tasks. Khonsu's replays were identical **24 of 24** times, with the model server switched off ([results](#results)).

## Get Khonsu

```sh
curl -fsSL https://lazeeez.github.io/Khonsu/downloads/install.sh | sh   # the CLI: Linux x86-64, or Windows via WSL
pip install khonsu-sdk                                                  # Python tool SDK + pytest plugin (import khonsu)
npm install khonsu-sdk                                                  # JavaScript / TypeScript tool SDK
```

- **The installer** downloads the package, verifies its SHA-256 checksum, and puts `khonsu` in `~/.local/bin`. The binary links the C++ runtime and OpenSSL statically, so it runs on any Linux with glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+, Fedora 36+).
- **The SDKs are optional.** Model calls are recorded with no code changes. The SDKs only wrap tools with side effects.
- **Files, checksums and platform notes** are on the [Download page](https://lazeeez.github.io/Khonsu/#/downloads). macOS is on the [roadmap](#roadmap).

<details>
<summary>Build from source (contributors)</summary>

Needs CMake ≥ 3.24, Clang ≥ 17 or GCC ≥ 13, Ninja and OpenSSL.

```bash
cmake --preset native && cmake --build --preset native     # build/native/khonsu, tests and benchmarks
scripts/test_native.sh native                              # every suite; also native-asan, native-tsan
cmake --preset wasm && cmake --build --preset wasm         # the WebAssembly kernel for the website (Emscripten)
cd web && npm ci && npm run dev                            # the website
```
</details>

## A tour in 60 seconds

Real output from the [support agent example](samples/support/agent.py): a customer's lamp arrived cracked, and an agent built on the OpenAI SDK and qwen2.5:3b handles the refund.

```console
$ khonsu record -- python agent.py                       # any framework: Khonsu sets OPENAI_BASE_URL
☾ khonsu recorded 11 calls (5 tools) · 13 workspace snapshots → support-agent.khn (41.9 KiB, fsync'd) · agent exit 0

$ khonsu replay support-agent.khn -- python agent.py      # model offline? doesn't matter
☾ khonsu replayed 11/11 from recording · 0 live · 0 divergences · workspace identical · agent exit 0

$ khonsu show support-agent.khn                           # every call, tool and file change
  calls      6 model + 5 tool · 3476 prompt + 480 completion tokens · 11.5 s in model calls
  workspace  /tmp/khonsu-demo/support · 13 snapshots · 5 objects (5.6 KiB, deduplicated)
  integrity  hash chain ok 25893cab732439b3

  #0 200  4507 ms  qwen2.5:3b
      in  Hi, I'm Dana Okafor. I ordered the Lunar Desk Lamp (order #A1042) two weeks ago and it arr…
      fn  lookup_order
  #1 tool 0.7 ms  __main__.lookup_order
      arg {"order_id":"A1042"}
      ret {"customer":"Dana Okafor","delivered":"2026-09-18","email":"dana.okafor@example.com","item…
  …
  #6 tool 0.6 ms  __main__.issue_refund                  # a real side effect: never repeated on replay
      arg {"amount":89,"order_id":"A1042","reason":"defective item"}
      ret {"amount":89.0,"order_id":"A1042","status":"refunded"}

$ khonsu verify support-agent.khn
frames     42 intact
hash chain ok
workspace  13 snapshots, 5 objects, all restorable
verified
```

When two runs of the same task disagree, `khonsu diff` lines them up call by call. Here is a coding agent that passed its tests, and the same agent run again, which never wrote the file:

```console
$ khonsu diff coding-agent.khn coding-agent-again.khn
  calls      A 12 · B 7 · 1 identical
  tokens     A 4078 · B 4906 (+828)

  first divergence A#1 / B#1 (qwen2.5:3b): same request, different output
  = A#0   B#0   qwen2.5:3b
  ~ A#1   B#1   qwen2.5:3b
  ≠ A#2   B#2   qwen2.5:3b  at messages[4].tool_calls
  …
```

Go back to the moment of the failure and let a new future play out:

```console
$ khonsu cat coding-agent.khn@6 solution.py              # any file, as it was just before call #6
$ khonsu fork coding-agent.khn@6 -- python agent.py      # restore the files, replay calls 0-5, go live from #6
```

All of these recordings, and the fork, open in the [Viewer](https://lazeeez.github.io/Khonsu/#/viewer?src=support-agent.khn), each with the story of what happened.

<details>
<summary>Every command</summary>

| Command | What it does |
|---|---|
| `khonsu record [-o FILE] [--upstream URL] -- CMD` | Run the agent through a recording proxy; snapshot the workspace before every call |
| `khonsu replay FILE [--on-miss fail\|live] [--realtime] -- CMD` | Serve every model and tool call from the recording; compare the workspace before every call |
| `khonsu fork FILE@N [--dir DIR] -- CMD` | Restore the workspace, replay calls before #N, go live from #N |
| `khonsu serve (--record FILE \| --replay FILE)` | A long-running proxy, for agents you start yourself |
| `khonsu checkout FILE@REF DIR` · `files FILE@REF` · `cat FILE@REF PATH` | The workspace at any moment (`REF` = `start`, `end` or a call number) |
| `khonsu show FILE` · `verify FILE` · `diff A B` | Inspect, check integrity, compare (`--json` for tooling) |
| `khonsu store add\|ls\|get\|rm\|serve` · `sync REMOTE` · `annotate` | Share runs and notes between machines |

The model API is `--upstream`, else `$KHONSU_UPSTREAM`, else your existing `$OPENAI_BASE_URL`, else `https://api.openai.com/v1`. For a free, local model, use `--upstream http://localhost:11434/v1` (Ollama).
</details>

## Highlights

- **Crash-safe by construction.** Every response is `fdatasync`'d before the agent can observe it. In 20 randomized `kill -9` trials, every response the agent had seen survived, and torn writes cut at ~400 offsets always recovered the valid prefix.
- **Bit-exact, order-independent replay.** Requests match on a hash of their canonical JSON plus an occurrence count, so parallel calls replay correctly in any order, and a drift names the exact field (`messages[3].content`).
- **Time travel for files, not just calls.** A content-addressed Merkle snapshot of the workspace before every call, inside the recording. On a 20,000-file repository, later snapshots take 34 ms and a one-file edit adds 10 KiB.
- **O(1) fork.** The engine's persistent Merkle store forks 64 MiB of state in **1.3 ns**, against 134 µs for a page-table design and 8.5 ms for a deep copy. Play forks a *running* program in about 3 µs.
- **Measured against the alternatives.** Against vcrpy, replay is 3.4× faster, recordings are 2.4× smaller, and 30 calls survive a `kill -9` where vcrpy keeps 0. Against LangGraph, 10 resumes give 1 outcome instead of 10, and 40 turns take 171 KiB instead of 2.1 MiB.
- **Found real bugs in real conditions.** A kill -9 test exposed a durability window in how the OpenAI SDK ends streams. ThreadSanitizer caught a listener shutdown race. The real-model study caught a replay bug in Khonsu's own tool semantics. All three are fixed and pinned by regression tests.
- **One engine, two targets.** The same C++23 kernel runs the CLI natively and the website in WebAssembly, so a `.khn` file is parsed and verified identically in your terminal and your browser.
- **Production-grade website.** 60 fps on every page with the CPU slowed 4×, about 0.3 s to first paint, tested in Chromium, Firefox and WebKit at phone, tablet and desktop sizes, with no serious accessibility violations (axe).

## Tools and SDKs

Model calls are recorded with no code changes. Tools live inside your program, so you mark the ones with side effects:

```python
import khonsu                                  # pip install khonsu-sdk

@khonsu.tool                                   # replay returns the recorded result: the email is not sent twice
def send_email(to: str, body: str) -> str: ...

@khonsu.tool(replay="rerun")                   # runs again for its effect; the agent still sees the recorded result
def write_file(path: str, content: str) -> int: ...
```

```ts
import { tool } from "khonsu-sdk";             // npm install khonsu-sdk

const sendEmail = tool("sendEmail", async (to: string, body: string) => { /* ... */ });
const writeFile = tool("writeFile", (path: string, text: string) => { /* ... */ }, { replay: "rerun" });
```

| Mode | On record | On replay | Use for |
|---|---|---|---|
| `skip` (default) | runs, result saved | not run; the saved result is returned | anything external: email, payments, APIs, the clock |
| `rerun` | runs, result saved | runs again for its effect; the saved result is returned and a difference is reported | writing files the agent reads back |

Both SDKs speak one small protocol to the recorder, so one recording can mix Python and Node tools. Errors are replayed as the same type, and a call whose arguments changed raises `KhonsuDivergence`, unless live fallback is on ([ADR 0006](docs/adr/0006-tool-replay-semantics.md)).

## Deterministic agent tests in CI

```python
import pytest

@pytest.mark.khonsu("recordings/refund_flow.khn")       # replayed: offline, free, same answer every run
def test_refund_flow():
    assert run_my_agent("I want a refund for order 1234").action == "refund"
```

```console
$ pytest --khonsu-record --khonsu-upstream http://localhost:11434/v1   # record once against a real model
$ pytest                                                              # every later run: replay
$ pytest    # after someone edits the system prompt:
FAILED tests/test_agent.py::test_refund_flow - agent behaviour diverged from recordings/refund_flow.khn
  request #0 /v1/chat/completions: expected recorded call #0, first difference at `messages[0].content`
    recorded: You are a careful support agent…
    now:      You are a friendly support agent…
  If the change is intended, re-record with: pytest --khonsu-record
```

## Share runs with your team

```console
$ khonsu store add .khonsu/runs/*.khn                         # content-addressed; workspaces deduplicated
$ khonsu annotate refund-bug@12 "hallucinated order id" --label root-cause
$ khonsu sync http://khonsu.internal:7400 --token T            # or a shared folder
☾ khonsu synced: 3/256 buckets differed · sent 5 objects (4.0 KiB) · received 2 (1.1 KiB) · 4 recordings, 3 notes
$ khonsu store get refund-bug -o refund-bug.khn                # byte-identical on any machine
```

- **Merkle anti-entropy.** Stores compare a 256-bucket Merkle summary, then exchange ids only for the buckets that differ, so the work scales with the difference. Pushing a fork of a shared run sent **4 KiB for a 1.37 MB recording**.
- **No coordinator, no conflicts.** Refs are a last-writer-wins map ordered by hybrid logical clocks, and notes are an observed-remove set, so any gossip order converges ([ADR 0008](docs/adr/0008-merkle-anti-entropy-and-crdt-state.md)).
- **Integrity end to end.** Every object is re-verified against its content id when read or received, and stores sync their directories after every rename, so a crash never leaves a ref pointing at a missing object.

## The website

**<https://lazeeez.github.io/Khonsu/>**: everything on it runs in your browser, on the same C++ engine compiled to WebAssembly. Nothing you open is uploaded.

| Page | What it is |
|---|---|
| [Home](https://lazeeez.github.io/Khonsu/) | The story: the problem, the evidence from the study, and what Khonsu does. |
| [Guide](https://lazeeez.github.io/Khonsu/#/guide) | What Khonsu is, a click-by-click tour of every page, how to use it on your own agent, concepts, the full CLI and an FAQ. |
| [Viewer](https://lazeeez.github.io/Khonsu/#/viewer?src=support-agent.khn) | Open a `.khn` recording or an OpenTelemetry GenAI trace (OTLP JSON from OpenLLMetry, OpenLIT, Langfuse, Phoenix…). The **Moon Dial** travels through the calls, each call shows as a readable transcript with highlighted code, the workspace shows file diffs per call, and two runs can be compared. Real examples (a support agent, a coding agent, a re-run and a fork) each come with the story of what happened. |
| [Play](https://lazeeez.github.io/Khonsu/#/play) | Fork a *running* program. Lune, a small deterministic VM, keeps its state in the engine's Merkle store: fork it mid-loop, edit a variable in the fork, run a dozen speculative futures, and watch which 4 KiB pages each one copies ([ADR 0009](docs/adr/0009-lune-forking-running-programs.md)). |
| [Simulator](https://lazeeez.github.io/Khonsu/#/simulator) | The engine underneath: a recorded multi-agent run to replay, fork, speculate, tamper with and verify, in six guided chapters. |
| [Benchmarks](https://lazeeez.github.io/Khonsu/#/bench) | Khonsu against vcrpy and LangGraph, plus the engine's own benchmark suite run live on your device. |
| [Download](https://lazeeez.github.io/Khonsu/#/downloads) | The one-line installer, the files with SHA-256 checksums, and the SDKs. |

Each tool page has a floating **?** with its how-to. How it's built:

- **Loads only what a page needs.** The landing page is 74 KiB of gzipped JavaScript with no WebAssembly. Other pages load on demand and are prefetched when the browser is idle or a link is hovered. The WebAssembly kernel (175 KiB gzipped) runs in a Web Worker, so the interface never blocks on it.
- **Smooth animation.** Everything animates with GPU-friendly properties only (transform and opacity), and nothing repaints the page every frame. The star field redraws at 20 fps and meteors only while one is in flight. The moon is a procedurally painted texture, generated once, with a soft terminator.
- **Robust.** Reveals are fail-safe, reduced-motion is respected, pages have their own titles, links have share previews, and broken links get a themed 404 page.

## Results

A real function-calling coding agent (qwen2.5:3b via Ollama, 8 tasks), and head-to-head comparisons with the tools teams use today. Method, per-task tables and caveats: **[docs/RESULTS.md](docs/RESULTS.md)**.

| | |
|---|---|
| Live re-runs of the same task that differed from the recorded run | **16 / 16**, and the tests flipped between pass and fail on 5 of 8 tasks |
| Replays identical to the recording, with the model server **stopped** | **24 / 24** |
| Editing the last prompt: tokens served from the recording | **76 %** (only the edited call goes to the model) |
| vs **vcrpy**: replay p50 · recording size · API key on disk · calls kept after `kill -9` at call 30 | **1.12** vs 3.77 ms · **186** vs 452 KiB · **no** vs yes · **30** vs 0 |
| vs **LangGraph** checkpointers: 10 resumes from mid-run · storage at 40 turns | **1** vs 10 distinct outcomes · **171 KiB** vs 2.1 MiB |

The study also found a bug in Khonsu itself: `rerun` tools returned fresh output, and pytest's `passed in 0.03s` made 11 of 24 replays miss. The fix and its regression test are described in the [ADR 0006 amendment](docs/adr/0006-tool-replay-semantics.md).

**The engine** (reference run: Node 24, wasm32, a laptop; the [Benchmarks page](https://lazeeez.github.io/Khonsu/#/bench) runs the same suite on your device):

| Operation | Time |
|---|---|
| `fork()` of 64 MiB of state (persistent Merkle tree: a root-handle copy) | **1.3 ns** |
| … the same fork with a copy-on-write page table / a deep copy | 134 µs / 8.5 ms |
| First write to a page after a fork (path copy + one 4 KiB page) | ~2 µs |
| Full `Kernel::fork` (timeline + CRC'd log frame) | 0.63 µs |
| Log append · CRC32C (slicing-by-8, no hardware CRC in WebAssembly) | 15.9 M frames/s · 2.05 GiB/s |
| Workspace snapshot of a 20,000-file repository (first / later) | 146 ms / 34 ms |

## Architecture

```text
                          ┌──────────────────────────── khonsu CLI (native, C++23) ────────────────────────────┐
 your agent ──HTTP──────► │  proxy (127.0.0.1)  ──TLS──►  model API (OpenAI, Anthropic, Ollama, vLLM, …)         │
  (any language,          │    │ canonical JSON → match key = XXH3(method, path, body) + occurrence              │
   any framework)         │    ▼                                                                                 │
 @khonsu.tool ──HTTP────► │  tool endpoint ──►  recording.khn   append-only · CRC32C frames · fsync'd · hash chain│
  (Python / JS SDK)       │                        ▲                                                             │
                          │  before every call:  workspace ──► Merkle snapshot (only new objects appended)       │
                          │  replay: serve by (key, occurrence) · miss ⇒ divergence at the first differing field │
                          │  store / sync: content-addressed objects · Merkle anti-entropy · CRDT refs + notes   │
                          └──────────────────────────────────────────────────────────────────────────────────────┘
                                         │ the same kernel, compiled to WebAssembly
                                         ▼
                          website (React + TypeScript) ─► Web Worker ─► kernel.wasm: parse, verify, diff, fork, Lune
```

**The kernel** (`kernel/`) is platform-free C++23: the event log and recording format, canonical JSON and matching, workspace snapshot trees, the run diff, packing and sync summaries, the CRDTs, the persistent Merkle store, the deterministic multi-agent runtime behind the Simulator, and the Lune VM. **The native platform** (`platform/native/`) adds sockets, TLS, files and processes to make the CLI. **The web platform** (`platform/web/`) exposes the kernel to JavaScript through a flat C interface.

Design principles, each with its decision record:

- **Record at the HTTP boundary.** Framework-agnostic by construction; the agent's code doesn't change ([ADR 0001](docs/adr/0001-record-at-the-http-boundary.md)).
- **Thread per connection.** Simple, correct streaming, verified under ThreadSanitizer ([ADR 0002](docs/adr/0002-thread-per-connection.md)).
- **Canonical JSON and content match keys.** Numbers keep their original lexemes (`0.10` is never rewritten as `0.1`), and matching is order-independent ([ADR 0003](docs/adr/0003-canonical-json-and-match-keys.md)).
- **Durable before observable.** If the agent saw a response, it's in the recording, including the SSE terminal event, which is held until it's on disk ([ADR 0004](docs/adr/0004-durable-before-observable.md)).
- **Workspace snapshots inside the recording.** A `.khn` file is self-contained. `.gitignore` is honoured, and `.env`, keys and credentials are never captured ([ADR 0005](docs/adr/0005-workspace-snapshots-inside-the-recording.md)).
- **Tool replay semantics.** `skip` and `rerun` ([ADR 0006](docs/adr/0006-tool-replay-semantics.md)).
- **Aligning runs for diff.** A longest common subsequence over (match key, occurrence) ([ADR 0007](docs/adr/0007-aligning-runs-for-diff.md)).
- **Merkle anti-entropy and CRDT state** ([ADR 0008](docs/adr/0008-merkle-anti-entropy-and-crdt-state.md)).
- **Lune, forking running programs** ([ADR 0009](docs/adr/0009-lune-forking-running-programs.md)).

Also by design:

- **Secrets never touch disk.** `Authorization` and API-key headers are recorded only as `<redacted>`.
- **Tamper-evident.** Every frame has a CRC32C, and the run's end frame commits to a hash chain over all frames, so `khonsu verify` catches bit-rot and rewritten history. Workspace objects are verified against their ids when a recording loads.
- **API coverage.** Streaming is recorded with chunk boundaries and timing. `show`, `diff` and the viewer understand OpenAI Chat Completions, OpenAI Responses and Anthropic Messages, buffered or streamed.

## Tech stack

| Area | Technology |
|---|---|
| Engine and CLI | C++23 (Clang 18, GCC 13), CMake + Ninja, POSIX sockets, OpenSSL (TLS), XXH3-128 ([xxHash](https://github.com/Cyan4973/xxHash)), CRC32C, no other dependencies |
| Browser engine | The same kernel compiled with Emscripten to WebAssembly (SIMD, native wasm exceptions), in a Web Worker |
| Website | React 19, TypeScript, Vite; Canvas 2D and SVG; no UI library; hosted on GitHub Pages |
| SDKs | Python (standard library only, plus a pytest plugin) and JavaScript/TypeScript (zero dependencies, Node 18+, bundled types) |
| Testing | A dependency-free C++ unit and property-test harness; Python integration suites driving the official `openai` SDK; Playwright on Chromium, Firefox and WebKit; axe-core; AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer |
| Delivery | GitHub Actions; portable static builds on Ubuntu 22.04; GitHub Releases; PyPI and npm with trusted publishing (OIDC, no stored tokens) |
| Research | Ollama + qwen2.5:3b on a laptop GPU; openai 3.20, vcrpy 8.3, LangGraph 1.2 |

## Repository layout

```text
kernel/            the C++23 engine (include/khonsu/*.hpp, src/*.cpp): log, recording, JSON, matching,
                   snapshots, run diff, pack/sync, CRDTs, persistent Merkle store, simulator runtime, Lune VM
platform/native/   the khonsu CLI: proxy, HTTP, TLS, file log, workspace, store and sync, commands
platform/web/      the WebAssembly bindings used by the website
sdk/python/        khonsu-sdk for Python (import khonsu) and the pytest plugin
sdk/js/            khonsu-sdk for JavaScript / TypeScript
web/               the website (src/) and its E2E, device, accessibility and performance tests (e2e/)
tests/             C++ unit tests, and Python integration suites (tests/integration/)
samples/           the agents behind the website's example recordings
study/, bench/     the real-model study and the vcrpy / LangGraph comparisons, with their raw results
scripts/           test, package, release and sample-recording scripts
docs/              the design document, architecture decision records, results
.github/workflows/ CI and site (pages.yml), the shared package build (package.yml), releases (release.yml)
```

## Correctness and quality

| Suite | What it proves |
|---|---|
| Unit + model-based property tests (60) | Persistent Merkle store, log, canonical JSON, matching, snapshot trees, pack/unpack, Merkle summaries, CRDT laws (3 replicas × 60 random histories), recording summaries for every supported API, and Lune: forking at random instructions reproduces the uninterrupted run, and one write after a fork copies one page |
| Integration (18), with the unmodified **official `openai` Python SDK** against a deliberately **non-deterministic** mock model | Live runs differ; replays are byte-identical with the model **offline**; 6 concurrent requests replay correctly; the API key is never on disk; divergence is pinpointed to the field; replay-then-live serves unchanged calls from the recording |
| **kill -9**, 20 randomized trials mid-stream · **torn writes**, cut at ~400 offsets · **tampering** | Every response the agent observed survived; 0 crashes and every valid prefix recovered; one flipped bit fails `verify`, and a CRC-valid rewrite breaks the hash chain |
| Workspace suite (25) | gitignore semantics; secrets never captured; `cat` and `checkout` at any call; every snapshot restorable; drifted files named on replay; forks restore files and run side effects past the fork point for real |
| Sync suite (17) | Dedup across runs; push and pull over a folder and over HTTP (byte-identical); forks send only new objects; concurrent notes converge; token auth; corrupt remote objects rejected |
| CI suite (10) | `diff` finds the right call and field; the pytest plugin records, replays offline, and fails a prompt change with the divergence report |
| JavaScript SDK suite (13) | A Node agent recorded and replayed offline: identical output, side effects not repeated, errors replayed as the same type, changed arguments raise `KhonsuDivergence` |
| **Package test** | The shipped binary contains no source paths; the installer refuses a tampered download; the installed CLI and SDK pass the integration suite |
| **Sanitizers** | Every suite under AddressSanitizer + UndefinedBehaviorSanitizer and ThreadSanitizer, and the kernel under WebAssembly in Node, on every push |
| **Website** | E2E through every page; a **device matrix** of 7 pages × Chromium, Firefox and WebKit × phone, tablet and desktop, with no errors and no horizontal overflow; an **axe** accessibility audit with no serious or critical issues; a performance check at 60 fps with the CPU slowed 4× |

```bash
scripts/test_native.sh native          # unit, integration, workspace, CI, sync and JS suites (also native-asan, native-tsan)
scripts/test_package.sh                # the downloadable package, end to end
cd web && node e2e/smoke.mjs && node e2e/devices.mjs && node e2e/a11y.mjs && node e2e/perf.mjs   # against a served build
```

## CI, releases and distribution

- **Every push**: all native suites (plain, ASan + UBSan, TSan); the WebAssembly build and its tests; the downloadable package, built on Ubuntu 22.04 for portability and tested end to end; the website's E2E, device-matrix and accessibility checks; then deployment to GitHub Pages with the fresh downloads.
- **Every version tag** (`scripts/release.sh 0.3.1`): the versions are checked to agree and the package is built and tested exactly as on every push. Then a GitHub Release is created with the downloads, checksums and notes, `khonsu-sdk` is published to PyPI, and to npm. Both registries use trusted publishing, so no token is stored anywhere. Every step is safe to repeat. Details for maintainers: **[RELEASING.md](RELEASING.md)**.
- **What ships**: the stripped CLI binary with its C++ runtime and OpenSSL linked in, and the two SDKs. The engine's source code is not distributed.

## Roadmap

| | Milestone | State |
|---|---|---|
| 1 | Recorder proxy and CLI: record, replay, replay-then-live, show, verify; durability and concurrency tests | ✅ done |
| 2 | Workspace snapshots in the Merkle store (`fork run@N` restores the files too); `@khonsu.tool` Python SDK | ✅ done |
| 3 | `khonsu diff` between runs; pytest plugin for deterministic agent tests in CI | ✅ done |
| 4 | Sync recordings between machines with Merkle anti-entropy; CRDT annotations | ✅ done |
| 5 | Web viewer: `.khn` and OpenTelemetry GenAI traces; the Moon Dial | ✅ done |
| 6 | Real-agent study on a local model; benchmarks against vcrpy and LangGraph ([results](docs/RESULTS.md)) | ✅ done |
| 7 | Fork a *running* program (the Lune VM); speculative branches; Play | ✅ done |
| 8 | Launch: the website's story, guide and real examples; accessibility and performance | ✅ done |
| 9 | Distribution: one-line installer, GitHub Releases, PyPI and npm; JavaScript SDK; Anthropic and OpenAI Responses support | ✅ done |
| 10 | Native macOS build | next |

## Documentation

- **[Guide](https://lazeeez.github.io/Khonsu/#/guide)**: using Khonsu, on the website.
- **[docs/RESULTS.md](docs/RESULTS.md)**: the study and the comparisons, with method and caveats.
- **[docs/adr/](docs/adr/)**: nine architecture decision records.
- **[docs/DESIGN.md](docs/DESIGN.md)**: the original design document for the engine.
- **[RELEASING.md](RELEASING.md)**: how releases work, and the one-time registry setup.
- **SDKs**: [Python](sdk/python/README.md) and [JavaScript / TypeScript](sdk/js/README.md).

## License

MIT. See [LICENSE](LICENSE).
