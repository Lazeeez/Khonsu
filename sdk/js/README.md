# khonsu-sdk (JavaScript / TypeScript)

The companion to the `khonsu` CLI, a time-travel debugger for AI agents, for agents written in Node.js.

```sh
npm install khonsu-sdk
curl -fsSL https://lazeeez.github.io/Khonsu/downloads/install.sh | sh    # the khonsu CLI (Linux, or Windows via WSL)
```

Guide, examples and downloads: **https://lazeeez.github.io/Khonsu/**

The CLI's proxy records model calls with no code changes. This package adds one wrapper for **tool side effects**. It has no dependencies, needs Node ≥ 18, and ships TypeScript types.

```ts
import { tool } from "khonsu-sdk";

const sendEmail = tool("sendEmail", async (to: string, body: string) => {
  ...  // the network, a database, the clock, the filesystem...
});

const writeFile = tool("writeFile", (path: string, text: string) => fs.writeFileSync(path, text), { replay: "rerun" });
```

| Under | Behaviour |
|---|---|
| `khonsu record -- node agent.js` | The tool runs, and its arguments and result (or thrown error) are recorded durably before it returns. |
| `khonsu replay run.khn -- node agent.js` | The recorded result is returned **without running the tool**, so side effects are not repeated and the output is reproducible. |
| `{ replay: "rerun" }` | On replay the tool runs again for its effect (a file the agent reads back). A changed result is reported, and the agent still gets the recorded one, so the replay stays on the recorded path. |
| `khonsu replay … --on-miss live` / `khonsu fork` | Calls whose arguments differ from the recording run for real and are recorded. |
| Plain `node agent.js` | The wrapper just calls the function. |

- A wrapped tool always returns a `Promise`. Arguments and results must be JSON-compatible; a result that is not (a `BigInt`, a cycle) throws `TypeError`, and the call is still closed in the recording.
- Errors are replayed as the same builtin type (`RangeError`, `TypeError`, …), or as `ToolError` for other types.
- If replay meets a call whose arguments are not in the recording, the tool throws `KhonsuDivergence`, unless live fallback is enabled.
- The name is part of the match key: keep it stable across runs.

It uses the same protocol as the Python SDK, so one recording can mix Python and Node tools.
