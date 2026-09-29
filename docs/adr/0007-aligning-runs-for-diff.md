# ADR 0007: Align runs by call identity (LCS) before comparing them

**Status:** accepted · 2026-09-30

## Context

`khonsu diff A B` has to answer "where did these two runs start to differ, and why?"
Comparing call *i* of A with call *i* of B fails as soon as one run makes one extra call:
every later pair then compares unrelated calls and the report becomes noise.

## Decision

- A call's identity is `(match key, occurrence)`, the same identity replay uses (ADR 0003).
- The two call sequences are aligned by their **longest common subsequence** of identities.
  O(n·m) time and memory is fine for runs of thousands of calls.
- Aligned pairs are `same` (same request, same output) or `same-request` (same request,
  different output: the model sampled differently). Output is compared as text plus tool
  calls *with arguments*; ids and timestamps in the raw body are ignored.
- Unaligned stretches between anchors are paired positionally as `changed`, and each gets the
  path of the first differing JSON field (for example `messages[2].tool_calls[0].function.arguments`).
  Leftovers are `only-a` / `only-b`.
- The first non-`same` call is the first divergence. Khonsu also reports which workspace files
  already differed just before it, and how the workspaces differ at the end.

## Consequences

- One extra or missing call shows up as a single `+`/`-` line, not as a cascade.
- A typical real divergence reads as a causal chain: `~` (the model chose different tool
  arguments) followed by `≠` (the next request differs because it feeds that result back).
- `diff` exits 0 when runs are identical and 1 otherwise, like `diff(1)`, so it can gate CI.

## Evidence

`tests/integration/run_ci.py`: two live runs diverge at call #0 as `same-request` (different
tool arguments), then call #1 as `changed` at `messages[…]`; a run and its replay diff as
identical.
