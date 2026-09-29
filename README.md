# ☾ Khonsu

**A time-travel debugger for AI agents.** Record any agent run once, then replay it bit-exactly, with zero tokens and zero network, as many times as you like. Change a prompt and pay only for what changed. When a replay drifts, Khonsu tells you the exact field that differs.

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

$ khonsu replay run.khn --on-miss live -o fork.khn -- python agent.py     # replay-then-live
☾ khonsu replayed 7/8 from recording · 1 live · 1 divergence → fork.khn
```

## Why

| Problem teams have with agents | What Khonsu does |
|---|---|
| **Failures can't be reproduced.** The model samples differently on every run, so the bug disappears. | `record` captures every model exchange durably (even if the process is `kill -9`'d). `replay` reproduces the run byte-for-byte. |
| **Agent tests are flaky and expensive.** Every CI run re-calls the model. | Commit a recording and replay it in CI: deterministic, offline, free. A behaviour change fails loudly with a field-level diff. |
| **Prompt iteration burns tokens.** Editing step 15 re-pays for steps 1–14. | `--on-miss live`: unchanged requests come from the recording; only changed ones go to the model. The result is recorded as a fork of the original. |

Existing tools cover pieces. Tracing SaaS (LangSmith, Langfuse, Phoenix) *shows* runs but can't *re-execute* them. HTTP cassettes (vcrpy) break on any prompt change. Framework checkpointers (LangGraph) are framework-locked and don't capture the workspace. Khonsu works with any language or framework, runs locally and is free.

## How it works

```text
 your agent ──OPENAI_BASE_URL──►  khonsu proxy (127.0.0.1) ──TLS──► model API (OpenAI, Ollama, vLLM, …)
                                    │   canonical JSON → match key = H(method, path, body) + occurrence
                                    ▼
                         recording.khn  ── append-only, CRC32C-framed, fsync'd, hash-chained
                                    │
                replay: serve by (key, occurrence) · miss ⇒ divergence at the first differing JSON field
```

- **Framework-agnostic by construction.** Recording happens at the HTTP boundary ([ADR 0001](docs/adr/0001-record-at-the-http-boundary.md)).
- **Exact, order-independent matching.** Requests are matched by a hash of their canonical JSON (numbers kept as original lexemes, so `0.10` is never rewritten as `0.1`) plus an occurrence count, so parallel requests replay correctly regardless of arrival order ([ADR 0003](docs/adr/0003-canonical-json-and-match-keys.md)).
- **Durable before observable.** A response is `fdatasync`'d before the agent can see it complete, so *if the agent saw it, it's in the recording*. Verified by kill -9 trials and torn-write tests ([ADR 0004](docs/adr/0004-durable-before-observable.md)).
- **Streaming-faithful.** SSE token streams are recorded with chunk boundaries and timing, and replayed instantly or in real time (`--realtime`).
- **Secrets never touch disk.** `Authorization` and API-key headers are recorded only as `<redacted>` (tested).
- **Tamper-evident.** Every frame has a CRC32C, and `RunEnd` commits to a hash chain over all frames, so `khonsu verify` catches bit-rot *and* rewritten history.

## Install and use

Linux (macOS and Windows via WSL are planned). Needs CMake ≥ 3.24, Clang ≥ 17 or GCC ≥ 13, Ninja, OpenSSL.

```bash
cmake --preset native && cmake --build --preset native
./build/native/khonsu record -o run.khn --upstream http://localhost:11434/v1 -- python agent.py   # Ollama: free, local
./build/native/khonsu show run.khn            # every call: model, prompt, output, tools, tokens, latency
./build/native/khonsu verify run.khn          # CRCs + hash chain
./build/native/khonsu replay run.khn -- python agent.py
./build/native/khonsu serve --replay run.khn  # long-running proxy; prints the OPENAI_BASE_URL to use
```

Upstream resolution: `--upstream`, else `$KHONSU_UPSTREAM`, else your existing `$OPENAI_BASE_URL`, else `https://api.openai.com/v1`.

## Correctness

| Suite | What it proves |
|---|---|
| Integration, with the unmodified **official `openai` Python SDK** against a deliberately **non-deterministic** mock model (random text, tool arguments, chunking, latency) | Live runs differ; replays are byte-identical 3/3 with the model **offline**; 6 concurrent requests replay correctly; the API key is never on disk; divergence is pinpointed to `messages[3].content`; replay-then-live serves 7 of 8 calls from the recording |
| **kill -9**, 12 randomized trials mid-stream | Every response the agent observed survived |
| **Torn writes**, cut at ~400 byte offsets | 0 crashes; every valid prefix recovered |
| **Tampering** | A single flipped bit fails `verify`; a CRC-valid rewrite breaks the hash chain |
| Unit + model-based property tests (37) | Persistent Merkle store, log, canonical JSON, matching |
| **ASan + UBSan, ThreadSanitizer**, Clang 18 and GCC 13, and WebAssembly under Node | Run in CI on every push. TSan caught a real listener shutdown race ([ADR 0002](docs/adr/0002-thread-per-connection.md)) |

```bash
python tests/integration/run_integration.py build/native/khonsu    # needs `pip install openai`
```

## The engine underneath

The recorder sits on a C++23 kernel that also compiles to WebAssembly. That kernel includes a persistent, content-addressed Merkle store (O(1) fork, 4 KiB copy-on-write pages, structural diff), a deterministic multi-agent runtime with verified replay, and an in-browser workbench.

- **[Live workbench](https://lazeeez.github.io/Khonsu/):** scrub, fork, speculate and verify a multi-agent run entirely in your browser.
- **[Design doc](docs/DESIGN.md)** and **[ADRs](docs/adr/)**.

## Roadmap

| | Milestone | State |
|---|---|---|
| 1 | Recorder proxy + CLI: record, replay, replay-then-live, show, verify; durability and concurrency tests | ✅ done |
| 2 | Workspace snapshots in the Merkle store (`fork run@N` restores the files too); `@khonsu.tool` Python SDK | next |
| 3 | `khonsu diff` between runs; pytest plugin for deterministic agent tests in CI | |
| 4 | Sync recordings between machines with Merkle anti-entropy; CRDT annotations | |
| 5 | Web viewer: drop a `.khn` or OpenTelemetry GenAI trace; the Moon Dial time scrubber | |
| 6 | Real-agent study on a local model; benchmarks vs vcrpy and LangGraph checkpointers | |
| 7 | Showcase: fork a *running* sandboxed program; speculative branches; Play mode | |

## License

MIT. See [LICENSE](LICENSE).
