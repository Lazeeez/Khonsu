# khonsu-sdk (Python)

The companion to the `khonsu` CLI, a time-travel debugger for AI agents.

```sh
pip install khonsu-sdk      # then: import khonsu
curl -fsSL https://lazeeez.github.io/Khonsu/downloads/install.sh | sh    # the khonsu CLI (Linux, or Windows via WSL)
```

Guide, examples and downloads: **https://lazeeez.github.io/Khonsu/**

Model calls are recorded by the CLI's proxy with no code changes. This package adds one decorator for **tool side effects**:

```python
import khonsu

@khonsu.tool
def run_tests(path: str) -> dict:
    ...   # touches the filesystem, the network, a database, the clock...
```

| Under | Behaviour |
|---|---|
| `khonsu record -- python agent.py` | The tool runs; its arguments and result (or exception) are recorded durably. |
| `khonsu replay run.khn -- python agent.py` | The recorded result is returned **without running the tool**: no repeated side effects, reproducible output. |
| `khonsu replay … --on-miss live` / `khonsu fork` | Calls whose arguments differ from the recording run for real and are recorded. |
| Plain `python agent.py` | The decorator does nothing. |

Arguments and return values must be JSON-compatible. Exceptions are replayed as the same builtin type (for example `ValueError`), or as `khonsu.ToolError` for other types. If replay meets a call whose arguments are not in the recording, it raises `khonsu.KhonsuDivergence`, with the differing field, unless live fallback is enabled.

## pytest plugin

Installed automatically (entry point `pytest11`). Mark a test with a recording:

```python
@pytest.mark.khonsu("recordings/refund_flow.khn")
def test_refund_flow():
    assert run_my_agent("I want a refund").action == "refund"
```

- `pytest` replays the recording: offline, free, same answer every time. If the agent's requests changed, the test fails with the call, the JSON field, and the old and new values.
- `pytest --khonsu-record [--khonsu-upstream URL]` (re-)records against a live model.
- The `khonsu` binary is found on `PATH`, in `$KHONSU_BIN`, or via `--khonsu-bin`. Create model clients inside the test so they pick up the replay server's base URL.

The SDK itself has no dependencies (standard library only); the plugin needs `pytest`.
