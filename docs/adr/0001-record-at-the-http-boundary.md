# ADR 0001: Record agents at the HTTP boundary, not inside a framework

**Status:** accepted · 2026-09-29

## Context

Khonsu has to make agent runs reproducible. The nondeterminism that matters most is the
model's output, and every popular agent stack (the OpenAI and Anthropic SDKs, LangGraph,
CrewAI, smolagents, aider, mini-swe-agent) reaches the model the same way: an HTTP call
to an OpenAI-compatible endpoint, with the base URL taken from `OPENAI_BASE_URL`.

The options were:

1. **Framework adapters** (a LangGraph checkpointer, a CrewAI hook, …): deep integration,
   but one adapter per framework, and every framework upgrade can break them.
2. **Monkey-patching the SDK**: Python-only, fragile across SDK versions, invisible to
   non-Python agents.
3. **An HTTP proxy**: set one environment variable and everything is recorded.

## Decision

Khonsu is a local HTTP proxy (`khonsu record -- CMD` injects `OPENAI_BASE_URL`). Tool side
effects that don't go over HTTP are captured by an optional one-line decorator
(`@khonsu.tool`, milestone 2), not by a framework integration.

## Consequences

- Works with any language and framework on day one, including ones that don't exist yet.
- Replay needs no cooperation from the agent: the agent's own code runs again, and only the
  world it talks to is replayed.
- The agent's *code* is not made deterministic. If it calls `time()` or `uuid4()` and feeds
  that into a prompt, replay reports a divergence at that field. Khonsu makes
  nondeterminism **visible and pinpointed**; it does not claim to remove it everywhere.
- Upstream TLS must be terminated by the proxy (it opens its own verified TLS connection),
  so secrets pass through a process on the user's machine. They are never written to disk
  (tested).

## Evidence

`tests/integration/run_integration.py` drives the unmodified official `openai` Python SDK
through the proxy: replay is byte-identical three times with the model server offline.
