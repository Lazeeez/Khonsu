# ADR 0004: A response is durable before the agent can observe it

**Status:** accepted · 2026-09-29

## Context

Recordings are most valuable exactly when things go wrong, including when the agent, the
machine, or Khonsu itself crashes mid-run. We need a crash guarantee that a user can
state in one sentence and that a test can check.

## Decision

The recording is an append-only log of CRC32C-framed records (the kernel's `EventLog`
format), written by `FileLog`:

- One `write()` per frame; `fdatasync()` (`F_FULLFSYNC` on macOS) to make it durable, and a
  directory `fsync` when the file is created.
- **Ordering rule:** the response frame is fsync'd *before* the agent can see a complete
  response. For buffered responses that means before the body is sent; for streams, before
  the terminating chunk, which is when SDKs consider a stream finished.
- **Recovery:** on open, keep the longest prefix of valid frames and truncate the rest (torn
  write). `RunEnd` carries a hash chain over every frame's CRC, so rewriting history is
  detectable even when every individual CRC is valid.

The guarantee: **if the agent observed a complete response, it is in the recording.**

## Consequences

- One `fdatasync` per model call. Against 300 ms to 60 s of model latency this is noise
  (sub-millisecond on NVMe). `--no-fsync` exists for tmpfs/CI scratch use.
- A request whose response was cut off by the crash is kept as a request without a response,
  and `khonsu show` flags it.

## Evidence

`tests/integration/run_integration.py`:

- **kill -9, 12 trials:** the recorder is SIGKILLed at a random moment while 4 parallel
  requests stream. Every response the agent printed is present in the recovered recording.
- **Torn writes:** a recording is cut at ~400 offsets. Every cut loads without crashing,
  and every cut past the first frame recovers the complete prefix.
- **Tampering:** a single flipped bit fails `khonsu verify`.
