# ☾ Khonsu

**A time-travel debugger for AI agents.** Record any agent run once, including the files it touched, then replay it bit-exactly, with zero tokens and zero network, as many times as you like. Jump to any call and see the workspace as it was. Fork from there and pay only for what changed. When a replay drifts, Khonsu names the exact field or file that differs.

Named after the Egyptian moon god whose name means *traveller*: Khonsu takes you back to the moment before things went wrong.

```console
$ khonsu record -- python agent.py                     # any framework: it's just OPENAI_BASE_URL
☾ khonsu recording to .khonsu/runs/run-20260929-213002.khn  upstream http://localhost:11434/v1
step1 tool_call get_weather(Memphis)
step2 answer Based on the tool result (Memphis: 27°C, clear skies), the answer is falcon-5828.
☾ khonsu recorded 2 exchanges → .khonsu/runs/run-20260929-213002.khn (6.5 KiB, fsync'd) · agent exit 0

$ khonsu replay .khonsu/runs/run-20260929-213002.khn -- python agent.py     # model offline? doesn't matter
☾ khonsu replayed 2/2 from recording · 0 live · 0 divergences · agent exit 0

$ khonsu replay run.khn -- python agent.py              # after a tool started returning something new
[khonsu] divergence at request #1 (POST /v1/chat/completions): recording expected request #1,
         first difference at `messages[3].content`
[khonsu]   recorded: Memphis: 27°C, clear skies
[khonsu]   now:      Memphis: 27°C, clear skies (stale)

$ khonsu show run.khn                                   # every call, tool and file change
  calls      2 model + 5 tool · 25 prompt + 32 completion tokens · 14 ms in model calls
  workspace  /tmp/demo-ws · 9 snapshots · 11 objects (538 B, deduplicated)
  #0 tool 0.5 ms  __main__.now_ns
      ret 1790694093626541845
  #1 200  5.4 ms  mock-1
      in  Write a haiku about Khonsu.
      out dune obelisk ember nile glyph ember #2312
  #2 tool 0.7 ms  __main__.write_file
      arg {"content":"# answer\ndune obelisk ember nile glyph ember #2312\n","path":"out/answer.md"}
      ws  +out/answer.md
  …
  #6 tool 0.4 ms  __main__.notify
      arg {"message":"agent finished"}

$ khonsu cat run.khn@2 out/answer.md                    # the file just before call #2…
khonsu: out/answer.md did not exist at @2
$ khonsu cat run.khn@3 out/answer.md                    # …and just after it
# answer
dune obelisk ember nile glyph ember #2312
$ khonsu checkout run.khn@start /tmp/before             # the whole workspace at any point

$ khonsu fork run.khn@4 -- python agent.py              # restore the files, replay calls 0-3, go live at #4
answer dune obelisk ember nile glyph ember #2312        # same as the original (replayed)
review lotus scarab obelisk scarab dune obelisk reed…   # regenerated live
☾ khonsu replayed 4/7 from recording · 3 live · 0 divergences · workspace identical → run-fork-at-4.khn
```

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

And when two runs disagree, `khonsu diff` aligns them call by call:

```console
$ khonsu diff yesterday.khn today.khn
  calls      A 2 · B 2 · 0 identical
  tokens     A 81 · B 115 (+34)

  first divergence A#0 / B#0 (mock-1): same request, different output
    A   → get_weather({"city": "Luxor"})
    B   → get_weather({"city": "Giza"})

  ~ A#0   B#0   mock-1
  ≠ A#1   B#1   mock-1  at messages[2].tool_calls[0].function.arguments
```

## See a run in your browser

**[Open the viewer →](https://lazeeez.github.io/Khonsu/#/viewer?src=support-agent.khn)** Drop any `.khn`, or an **OpenTelemetry GenAI trace** (OTLP JSON from OpenLLMetry, OpenLIT, Langfuse, Phoenix…). It is parsed by the same C++ kernel, compiled to WebAssembly, so nothing leaves your machine.

- **The Moon Dial**: the run is an orbit, each call a point on it, and the moon waxes as you travel from the first call to the last. Drag, click, or use the arrow keys.
- **Every call in full**: the prompt as a transcript, the response, tool calls, and stream chunk timing.
- **The workspace at that moment**: files before the call, and a diff of exactly what the call changed.
- **Real examples**: a support agent handling a refund, and a coding agent that fails a test and fixes it (plus a re-run and a fork), all recorded from qwen2.5:3b by `scripts/make_samples.py`. Each opens with the story of what happened.
- **Compare two runs**: the aligned run diff, with the first divergence explained.
- **Integrity at a glance**: hash chain verified, torn tails recovered, forks linked to their parent.

The site is code-split: the landing page ships no WebAssembly, and the viewer (7 KB gzipped plus the 400 KB kernel) loads on demand.

## Share runs with your team

```console
$ khonsu store add .khonsu/runs/*.khn                         # content-addressed; workspaces deduplicated
$ khonsu annotate refund-bug@12 "hallucinated order id" --label root-cause
$ khonsu sync /mnt/team-share/khonsu                           # or: khonsu sync http://khonsu.internal:7400
☾ khonsu synced with /mnt/team-share/khonsu: 3/256 buckets differed · sent 5 objects (4.0 KiB) · received 2 (1.1 KiB) · 4 recordings, 3 notes
$ khonsu store get refund-bug -o refund-bug.khn                # byte-identical on any machine
```

- **Merkle anti-entropy.** Stores compare a 256-bucket Merkle summary, then exchange ids only for the buckets that differ, so work scales with the difference: already-synced stores settle in one root comparison. Pushing a fork of a shared run sent **4 KiB for a 1.37 MB recording**.
- **No coordinator, no conflicts.** Refs are a last-writer-wins map ordered by hybrid logical clocks; notes are an observed-remove set (a concurrent add is never lost). Merges are commutative, associative and idempotent, so any gossip order converges ([ADR 0008](docs/adr/0008-merkle-anti-entropy-and-crdt-state.md)).
- **Integrity end to end.** Every object is re-verified against its content id when read or received. A corrupted remote object stops the pull and is never stored.

## Why

| Problem teams have with agents | What Khonsu does |
|---|---|
| **Failures can't be reproduced.** The model samples differently on every run, so the bug disappears. | `record` captures every model exchange durably (even if the process is `kill -9`'d). `replay` reproduces the run byte-for-byte. |
| **Agent tests are flaky and expensive.** Every CI run re-calls the model. | `@pytest.mark.khonsu(...)`: commit a recording and the test replays it: deterministic, offline, free. A behaviour change fails with a field-level report. `khonsu diff` explains how two runs differ. |
| **Prompt iteration burns tokens.** Editing step 15 re-pays for steps 1–14. | `--on-miss live` or `fork run@N`: unchanged requests come from the recording; only changed ones go to the model. The result is recorded as a fork of the original. |
| **Agents have side effects.** Re-running one re-sends the email and re-writes the files. | `@khonsu.tool` records tool calls. Replay returns the recorded result without re-running the side effect. Every call snapshots the workspace, so any point can be checked out or forked. |

Existing tools cover pieces. Tracing SaaS (LangSmith, Langfuse, Phoenix) *shows* runs but can't *re-execute* them. HTTP cassettes (vcrpy) break on any prompt change. Framework checkpointers (LangGraph) are framework-locked and don't capture the workspace. Khonsu works with any language or framework, runs locally and is free.

## How it works

```text
 your agent ──OPENAI_BASE_URL──►  khonsu proxy (127.0.0.1) ──TLS──► model API (OpenAI, Ollama, vLLM, …)
                                    │   canonical JSON → match key = H(method, path, body) + occurrence
                                    ▼
                         recording.khn  ── append-only, CRC32C-framed, fsync'd, hash-chained
                                    │
                replay: serve by (key, occurrence) · miss ⇒ divergence at the first differing JSON field

 before every call:  workspace ──► Merkle snapshot (blobs + per-directory trees, gitignore-aware)
                                    only new objects are appended to the recording
```

- **Framework-agnostic by construction.** Recording happens at the HTTP boundary ([ADR 0001](docs/adr/0001-record-at-the-http-boundary.md)).
- **Exact, order-independent matching.** Requests are matched by a hash of their canonical JSON (numbers kept as original lexemes, so `0.10` is never rewritten as `0.1`) plus an occurrence count, so parallel requests replay correctly regardless of arrival order ([ADR 0003](docs/adr/0003-canonical-json-and-match-keys.md)).
- **Durable before observable.** A response is `fdatasync`'d before the agent can see it complete, so *if the agent saw it, it's in the recording*. Verified by kill -9 trials and torn-write tests ([ADR 0004](docs/adr/0004-durable-before-observable.md)).
- **Streaming-faithful.** SSE token streams are recorded with chunk boundaries and timing, and replayed instantly or in real time (`--realtime`). `show` and the viewer understand OpenAI Chat Completions, OpenAI Responses and Anthropic Messages, buffered or streamed.
- **Secrets never touch disk.** `Authorization` and API-key headers are recorded only as `<redacted>` (tested).
- **Tamper-evident.** Every frame has a CRC32C, and `RunEnd` commits to a hash chain over all frames, so `khonsu verify` catches bit-rot *and* rewritten history.
- **Workspace time travel.** Before every call the working directory is captured as a content-addressed Merkle tree *inside* the recording, so a `.khn` file is self-contained. Unchanged files and subtrees are shared: on a 20,000-file repo, a later snapshot takes 34 ms and a one-file edit adds 10 KiB. `.gitignore` is honoured, and `.env`, keys and credential files are never captured ([ADR 0005](docs/adr/0005-workspace-snapshots-inside-the-recording.md)).
- **Tools without repeated side effects.** `@khonsu.tool` (Python, standard library only) returns recorded results on replay. `replay="rerun"` runs workspace-writing tools again for their effect, reports a changed result, and hands the agent the recorded one, so the replay stays on the recorded path ([ADR 0006](docs/adr/0006-tool-replay-semantics.md)).

## Install and use

Linux (macOS and Windows via WSL are planned). Needs CMake ≥ 3.24, Clang ≥ 17 or GCC ≥ 13, Ninja, OpenSSL.

```bash
cmake --preset native && cmake --build --preset native
./build/native/khonsu record -o run.khn --upstream http://localhost:11434/v1 -- python agent.py   # Ollama: free, local
./build/native/khonsu show run.khn            # every call: model, prompt, output, tools, tokens, latency
./build/native/khonsu verify run.khn          # CRCs + hash chain
./build/native/khonsu replay run.khn -- python agent.py
./build/native/khonsu serve --replay run.khn  # long-running proxy; prints the OPENAI_BASE_URL to use
./build/native/khonsu files run.khn@3         # workspace listing before call #3
./build/native/khonsu fork run.khn@3 -- python agent.py
pip install ./sdk/python                      # @khonsu.tool (Python)
npm install ./sdk/js                          # tool() for Node/TypeScript agents, same protocol
```

```python
import khonsu

@khonsu.tool                      # replay returns the recorded result; the email is not sent twice
def send_email(to: str, body: str) -> str: ...

@khonsu.tool(replay="rerun")      # runs again for its effect; the agent still sees the recorded result
def write_file(path: str, content: str) -> int: ...
```

Upstream resolution: `--upstream`, else `$KHONSU_UPSTREAM`, else your existing `$OPENAI_BASE_URL`, else `https://api.openai.com/v1`.

## Results

On a real function-calling coding agent (qwen2.5:3b via Ollama, 8 tasks), and against the tools teams use today. The method, per-task tables and caveats are in **[docs/RESULTS.md](docs/RESULTS.md)**.

| | |
|---|---|
| Live re-runs of the same task that differed from the recorded run | **16 / 16**; the pass/fail outcome flipped on 5 of 8 tasks |
| Replays identical to the recording, with the model server **stopped** | **24 / 24** |
| Editing the last prompt: tokens served from the recording | **76 %** (only the edited call goes to the model) |
| vs **vcrpy**: replay p50 · size · API key on disk · calls kept after `kill -9` at call 30 | 1.12 vs 3.77 ms · 186 vs 452 KiB · no vs yes · 30 vs 0 |
| vs **LangGraph** checkpointers: 10 resumes from mid-run · storage at 40 turns | 1 vs 10 distinct outcomes · 171 KiB vs 2.1 MiB |

The study also found a bug in Khonsu itself: `rerun` tools returned fresh output, and pytest's `passed in 0.03s` made 11 of 24 replays miss. The fix and a regression test are described in the [ADR 0006 amendment](docs/adr/0006-tool-replay-semantics.md).

## Correctness

| Suite | What it proves |
|---|---|
| Integration, with the unmodified **official `openai` Python SDK** against a deliberately **non-deterministic** mock model (random text, tool arguments, chunking, latency) | Live runs differ; replays are byte-identical 3/3 with the model **offline**; 6 concurrent requests replay correctly; the API key is never on disk; divergence is pinpointed to `messages[3].content`; replay-then-live serves 7 of 8 calls from the recording |
| **kill -9**, 20 randomized trials mid-stream | Every response the agent observed survived. This test caught a real window: the OpenAI SDK finishes a stream at `[DONE]`, before the HTTP terminator, so the chunk carrying the terminal event is now held until durable ([ADR 0004](docs/adr/0004-durable-before-observable.md)) |
| **Torn writes**, cut at ~400 byte offsets | 0 crashes; every valid prefix recovered |
| **Tampering** | A single flipped bit fails `verify`; a CRC-valid rewrite breaks the hash chain |
| Workspace suite (25 checks) | gitignore semantics; secrets never captured; `cat`/`checkout` at any call; every snapshot restorable; replay verifies the workspace before every call and names drifted files; forks restore files, replay the prefix and run side effects past the fork point for real; a `rerun` tool with a timing in its output still replays exactly |
| Sync suite (17 checks) | dedup across runs; push/pull over a directory and HTTP (byte-identical); fork push sends only new objects; one-comparison no-op sync; concurrent notes converge; LWW refs and tombstones; token auth; corrupted remote objects rejected |
| CI suite (10 checks) | `diff`: live runs diverge at the right call and field; a run and its replay are identical. pytest plugin: records, then replays offline and deterministically; a prompt change fails with the divergence report; a missing recording fails with instructions |
| Unit + model-based property tests (60) | Persistent Merkle store, log, canonical JSON, matching, snapshot trees, pack/unpack, Merkle summaries, CRDT semilattice laws (3 replicas × 60 random histories); Lune: forking at random instructions reproduces the uninterrupted run, one write after a fork copies one page |
| JavaScript SDK suite (13 checks) | A Node agent recorded, then replayed with the model offline: identical output; side effects not repeated; `rerun` tools run again but return the recorded result; errors replay as the same type; changed arguments raise `KhonsuDivergence` |
| Web: E2E plus a **device matrix** | Every page in Chromium, Firefox and WebKit (Safari's engine) at phone, tablet and desktop sizes: no page or console errors, no horizontal overflow, and the WebAssembly worker runs in each engine |
| **ASan + UBSan, ThreadSanitizer**, Clang 18 and GCC 13, and WebAssembly under Node | Run in CI on every push. TSan caught a real listener shutdown race ([ADR 0002](docs/adr/0002-thread-per-connection.md)) |

```bash
python tests/integration/run_integration.py build/native/khonsu    # needs `pip install openai`
```

## The engine underneath

The recorder sits on a C++23 kernel that also compiles to WebAssembly. That kernel includes a persistent, content-addressed Merkle store (O(1) fork, 4 KiB copy-on-write pages, structural diff), a deterministic multi-agent runtime with verified replay, and an in-browser workbench.

- **[Play](https://lazeeez.github.io/Khonsu/#/play):** fork a *running* program. Lune, a small deterministic VM, keeps its whole state in the Merkle store: a fork of a committed VM takes about 3 µs whatever its size, the child copies a 4 KiB page only when it first writes it, and one click runs a dozen speculative futures ([ADR 0009](docs/adr/0009-lune-forking-running-programs.md)).
- **[Live workbench](https://lazeeez.github.io/Khonsu/#/simulator):** scrub, fork, speculate and verify a multi-agent run entirely in your browser.
- **[Design doc](docs/DESIGN.md)** and **[ADRs](docs/adr/)**.

## Roadmap

| | Milestone | State |
|---|---|---|
| 1 | Recorder proxy + CLI: record, replay, replay-then-live, show, verify; durability and concurrency tests | ✅ done |
| 2 | Workspace snapshots in the Merkle store (`fork run@N` restores the files too); `@khonsu.tool` Python SDK | ✅ done |
| 3 | `khonsu diff` between runs; pytest plugin for deterministic agent tests in CI | ✅ done |
| 4 | Sync recordings between machines with Merkle anti-entropy; CRDT annotations | ✅ done |
| 5 | Web viewer: drop a `.khn` or OpenTelemetry GenAI trace; the Moon Dial time scrubber | ✅ done |
| 6 | Real-agent study on a local model; benchmarks vs vcrpy and LangGraph checkpointers ([results](docs/RESULTS.md)) | ✅ done |
| 7 | Showcase: fork a *running* program (Lune VM); speculative branches; Play mode | ✅ done |
| 8 | Launch: landing story from the study, results docs, final README | ✅ done |

## License

MIT. See [LICENSE](LICENSE).
