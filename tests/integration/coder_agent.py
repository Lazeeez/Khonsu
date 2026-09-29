"""A tiny "coding agent" that works in its current directory (the workspace).

It mixes every kind of nondeterminism and side effect Khonsu has to handle:
  - model calls (OpenAI SDK)                      → recorded by the proxy
  - the clock via a tool                          → @khonsu.tool: replayed, not re-read
  - an external side effect (a notification)      → @khonsu.tool: skipped on replay
  - workspace writes the agent reads back         → @khonsu.tool(replay="rerun")
  - a rerun check whose report varies (timings)    → rerun, but the recorded report is returned
"""

import json
import os
import time

import khonsu
from openai import OpenAI


@khonsu.tool
def now_ns() -> int:
    return time.time_ns()


@khonsu.tool
def notify(message: str) -> str:
    with open(os.environ["SIDE_EFFECTS"], "a") as f:  # outside the workspace, like sending an email
        f.write(message + "\n")
    return "sent"


@khonsu.tool(replay="rerun")
def write_file(path: str, content: str) -> int:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    return len(content)


@khonsu.tool(replay="rerun")
def lint(path: str) -> str:
    # a deterministic check with a nondeterministic report, like pytest's "passed in 0.03s"
    t = time.perf_counter_ns()
    n = len(open(path).read().splitlines())
    return f"{path}: {n} lines ok ({time.perf_counter_ns() - t} ns, pid {os.getpid()})"


client = OpenAI(max_retries=0)
started = now_ns()
task = open("task.txt").read()
answer = client.chat.completions.create(model="mock-1", messages=[{"role": "user", "content": task}])
write_file("out/answer.md", f"# answer\n{answer.choices[0].message.content}\n")
write_file("out/meta.json", json.dumps({"started_ns": started}))
review = client.chat.completions.create(
    model="mock-1", messages=[{"role": "user", "content": "review this:\n" + open("out/answer.md").read()
                                        + "\nlint: " + lint("out/answer.md")}])
write_file("out/review.md", review.choices[0].message.content + "\n")
notify("agent finished")
print("answer", open("out/answer.md").read().splitlines()[1])
print("review", open("out/review.md").read().strip())
print("started", started)
