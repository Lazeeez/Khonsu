# ADR 0006: Tools are skipped on replay by default; workspace tools opt into re-running

**Status:** accepted · 2026-09-29

## Context

Agents act through tools, and tools are where most nondeterminism and all side effects
live: the clock, the network, databases, notifications, shell commands, file writes. On
replay each tool call has two possible behaviours, and each is wrong for some tools:

- **Skip it and return the recorded result.** This is right for external effects: replaying an
  incident must not send the email again or charge a card twice. But if the tool wrote a file
  the agent later reads, skipping it leaves the workspace wrong.
- **Run it again.** This is right for local, deterministic effects on the workspace, and wrong
  for anything external or nondeterministic.

## Decision

`@khonsu.tool` (Python SDK, standard library only) talks to the proxy over a two-step protocol:
`begin {name, args}` → `replay {outcome}` or `execute {token}`, then `end {token, outcome}`.

- **Default, `replay="skip"`:** the recorded result (or exception) is returned and the function
  is not called.
- **`replay="rerun"`:** the function runs again, and a result that differs from the recording
  is reported. Khonsu's workspace snapshots independently verify the files it produced.
- Arguments and results are canonical JSON, and they are matched exactly like model requests
  (ADR 0003): a call with different arguments is a divergence, or goes live under
  `--on-miss live` / `fork`.
- Exceptions are recorded as `{type, message}` and replayed as the same builtin type, or as
  `khonsu.ToolError`.
- The tool's outcome is made durable before the tool returns to the agent (ADR 0004).

## Consequences

- The safe choice is the default: nothing external happens twice on replay.
- Authors mark workspace-writing tools `rerun`. Getting it wrong is visible: replay reports a
  workspace mismatch naming the files.
- Tools that aren't decorated still run live on replay. If they make a run diverge, the model
  request or workspace check pinpoints where.

## Evidence

`tests/integration/run_workspace.py` uses a clock tool (replayed: identical timestamps), a
notification tool with an external side effect (not repeated on replay; repeated in a fork past
the fork point), and a `rerun` file-writing tool (workspace verified identical at every call).
