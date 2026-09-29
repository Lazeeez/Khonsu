# ADR 0003: Match requests by a canonical-JSON hash plus an occurrence number

**Status:** accepted · 2026-09-29

## Context

On replay, each incoming request must be paired with the recorded response it should get.
Candidate strategies:

| Strategy | Failure mode |
|---|---|
| By arrival order | Parallel requests arrive in a different order every run |
| By raw body hash | Key order and whitespace differ between SDK versions and languages |
| By "semantic" similarity | Silently serves a wrong response: the worst outcome for a debugger |

## Decision

- **Match key** = XXH3-128 over `(method, path, canonical(body))`.
- **Canonical JSON**: object keys sorted bytewise, no whitespace, strings re-escaped with one
  fixed policy, and **numbers kept as their original lexemes**. We never round-trip a number
  through `double`, so `0.10`, `1e-5` and `-0` hash the same on every machine and in every
  library version. This is why Khonsu has its own small JSON parser (`kernel/src/jsonv.cpp`)
  instead of a general-purpose one.
- **Occurrence**: the n-th request with the same key gets the n-th recorded response with that
  key. Identical requests (retries, repeated side calls) replay in order, and unrelated
  concurrent requests match regardless of arrival order.
- **A miss is a divergence.** Khonsu reports the path of the first JSON field that differs
  from the request the recording expected next (for example `messages[3].content`), with both
  values. Then it fails (`--on-miss fail`) or forwards to the model (`--on-miss live`).

## Consequences

- Exact matching never serves a response to the wrong question.
- Nondeterministic values in prompts (timestamps, UUIDs) cause divergences by design. A
  future `--ignore-field` option can mask known-volatile paths explicitly.
- If two *identical* requests are in flight at the same moment, which one gets occurrence 0
  depends on arrival order. They asked the same question, so either answer is a valid replay.

## Evidence

Unit tests (`tests/test_jsonv.cpp`, `tests/test_recording.cpp`) cover key order, whitespace,
number lexemes, surrogate pairs, occurrence ordering and field-level divergence paths. The
integration suite replays 6 concurrent side requests byte-identically.
