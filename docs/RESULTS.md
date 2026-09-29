# Results

What Khonsu changes for a real agent, and how it compares with the tools teams use today.
Every number here comes from a script in this repository, and the raw output is committed
next to it. The Benchmarks page of the web app shows the same files.

| Study | Script | Raw output |
|---|---|---|
| A real agent on a local model | [`study/run_study.py`](../study/run_study.py) | [`study/results.json`](../study/results.json) |
| Khonsu vs vcrpy (HTTP cassettes) | [`bench/compare/bench_vcr.py`](../bench/compare/bench_vcr.py) | [`bench/results/vcr.json`](../bench/results/vcr.json) |
| Khonsu vs LangGraph checkpointers | [`bench/compare/bench_langgraph.py`](../bench/compare/bench_langgraph.py) | [`bench/results/langgraph.json`](../bench/results/langgraph.json) |

**Machine:** a laptop with an Intel i5-13420H and an RTX 4050 Laptop GPU (6 GiB), running Ubuntu 24.04
under WSL2. **Software:** Python 3.12, openai 3.20, vcrpy 8.3.0, langgraph 1.2.12 with
langchain-openai 1.6.6 and langgraph-checkpoint-sqlite 3.1.1, and Ollama 0.34.4 serving qwen2.5:3b.
Everything is free and runs offline.

## 1. A real agent on a local model

**The agent** ([`study/agent.py`](../study/agent.py)) is a small function-calling coding agent.
It is given a task, writes `solution.py` with a `write_file` tool, runs hidden tests with a
`run_tests` tool, fixes the code until the tests pass (at most 5 rounds), and then writes a
one-line changelog summary. Both tools are `@khonsu.tool(replay="rerun")`.
**The tasks** ([`study/tasks.py`](../study/tasks.py)) are 8 small programming problems:
fizzbuzz, palindrome, roman numerals, anagram groups, flatten, word count, brackets and
interval merging.

For each task the study:

1. **records** one run with `khonsu record`, then runs the same task **live twice more**;
2. **stops Ollama** (so no model is reachable at all) and **replays** the recording 3 times,
   each time in a fresh copy of the workspace;
3. restarts Ollama, **edits only the final summary prompt**, and runs
   `khonsu replay --on-miss live`, so unchanged calls come from the recording and the rest go
   to the model.

| task | recorded run | live re-run 1 | live re-run 2 | replays, model stopped | prompt edit: calls from the recording |
|---|---|---|---|---|---|
| fizzbuzz | fail · 6 calls | pass | fail | 3/3 identical | 5/6 |
| palindrome | pass · 5 calls | pass | fail | 3/3 identical | 4/5 |
| roman | fail · 6 calls | fail | fail | 3/3 identical | 5/6 |
| anagrams | fail · 6 calls | fail | fail | 3/3 identical | 5/6 |
| flatten | pass · 3 calls | fail | fail | 3/3 identical | 2/3 |
| wordcount | fail · 6 calls | fail | fail | 3/3 identical | 5/6 |
| brackets | pass · 4 calls | pass | fail | 3/3 identical | 3/4 |
| intervals | fail · 6 calls | fail | pass | 3/3 identical | 5/6 |

A replay counts as **identical** when the code it produced, the summary and the test outcome
all equal the recording's. Summary:

| | |
|---|---|
| Live re-runs whose code or summary differed from the recorded run | **16 / 16** |
| Tasks whose pass/fail outcome flipped between identical live runs | **5 / 8** |
| Replays identical to the recording, with the model server stopped | **24 / 24** |
| Prompt edit: model calls served from the recording | 34 / 42 (only the edited call went live) |
| Prompt edit: tokens not re-paid | **76 %** (27,152 of 35,892) |
| Mean wall time: live run vs replay | 17.3 s vs 3.0 s (the replay time is mostly pytest, which `rerun` executes again) |

**What this shows.** On a small model a coding agent is a coin toss: the same task on the
same prompt passes or fails depending on the sample, so a failure seen once is usually gone
on the next run. A recording makes that one failing run permanent. It replays exactly, offline,
at no cost, and it can be forked at the call that went wrong.

**A bug the study found in Khonsu.** The first full run found only **13 of 24** replays
identical. `run_tests` is a `rerun` tool, and pytest ends its report with `2 passed in 0.03s`.
On replay the tool ran again, the timing differed, and so the next model request differed
from the recording and replay stopped at a miss. The fix changed the `rerun` contract: the
tool still runs again for its effect and a changed result is reported, but the agent receives
the recorded result ([ADR 0006, amendment](adr/0006-tool-replay-semantics.md)). A regression
check was added to the workspace suite (a `rerun` lint tool whose report embeds a timing and
a PID). The raw output of the failing run is not committed. It is reproducible from the commit
before the fix.

**Caveats.** Eight tasks and one 3B model make a small sample. The pass rate (3 of 8
recorded runs) measures qwen2.5:3b, not Khonsu. The per-task nondeterminism is the point.
Small models sometimes wrap tool arguments in the schema (`{"content": {"type": "string",
"content": ...}}`), and the agent unwraps that before calling the tool.

## 2. Khonsu vs vcrpy

[vcrpy](https://github.com/kevin1024/vcrpy) records HTTP interactions into YAML "cassettes"
and is what most Python teams reach for. The same agent script (the official OpenAI SDK, 300
sequential calls against the local non-deterministic test model) runs direct, under Khonsu,
and under vcrpy.

| | Khonsu | vcrpy |
|---|---|---|
| Record, p50 per call (direct: 1.19 ms) | 2.66 ms | 2.91 ms |
| Replay / playback, p50 per call | **1.12 ms** | 3.77 ms |
| Recording size, 300 calls | **186 KiB** | 452 KiB |
| API key written to the recording (default settings) | **no** | yes |
| `kill -9` after 30 completed calls: calls in the recording | **30** | 0 (the cassette is written at exit) |
| Prompt edited at call 15 of 20 | 15 replayed, 5 live, change reported at `messages[0].content` | the run fails (`record_mode="none"`: no match) |

vcrpy can filter headers and can be set to re-record new interactions, so the key and the
prompt-change rows describe defaults, not hard limits. The durability row is structural:
Khonsu appends and syncs each exchange before the agent can observe it ([ADR 0004](adr/0004-durable-before-observable.md)).

## 3. Khonsu vs LangGraph checkpointers

LangGraph's "time travel" resumes a graph from a stored checkpoint. That is built for
*branching* (continue from here with new inputs), not *reproducing*: resuming restores the
state and then calls the model again. The benchmark uses the prebuilt ReAct agent with a
tool that records every call as a side effect, against the non-deterministic test model.

| | Khonsu | LangGraph |
|---|---|---|
| Resume/replay from mid-run, 10 times: distinct final outcomes | **1** | 10 |
| Storage after 40 turns | **171 KiB** (`.khn`) | 2.1 MiB (SqliteSaver) |
| 10-turn run, time on top of InMemorySaver (72 ms) | +23 ms (recording through Khonsu) | +60 ms (SqliteSaver) |
| Works with other frameworks | yes (the HTTP boundary) | no |
| Captures the workspace files | yes, per call | no |

The two compose. A LangGraph agent can be recorded by Khonsu unchanged, since it is just
`OPENAI_BASE_URL`, and keep its own checkpointer for branching.

## 4. Forking a running program (Play)

The Play page runs Lune, a small deterministic VM, in the browser. Its whole state (pc,
stack, variables and arrays in 4 KiB pages) is committed into the same persistent Merkle
store the recorder uses ([ADR 0009](adr/0009-lune-forking-running-programs.md)). The numbers
are live on the page. Typical values in Chromium on the machine above:

- **fork of an already-committed VM: about 3 µs**, independent of state size (a root copy plus
  restoring the small scalar state);
- the first fork writes only the parent's dirty pages (e.g. 13 pages mid-run on the ledger
  program), not its 2 MiB of arrays;
- after the child runs to completion, its page map against the fork point shows 104 pages
  still physically shared, 1 copied and 407 new: exactly the journal it wrote after the fork.

Unit tests fork at random instructions and check that parent and child both finish with the
uninterrupted run's output, that a write after a fork copies exactly one page, and that
`load(commit())` resumes the same computation.

## Reproducing

```bash
pip install openai vcrpy langgraph langchain-openai langgraph-checkpoint-sqlite pytest
cmake --preset native && cmake --build --preset native
python bench/compare/bench_vcr.py        # writes bench/results/vcr.json
python bench/compare/bench_langgraph.py  # writes bench/results/langgraph.json
ollama pull qwen2.5:3b && python study/run_study.py build/native/khonsu   # writes study/results.json (~15 min)
```
