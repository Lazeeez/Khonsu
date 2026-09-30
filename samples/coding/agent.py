"""A coding agent: function calling against any OpenAI-compatible model.

The model writes solution.py with write_file, runs the hidden tests with run_tests, and
fixes its code until they pass (at most 5 rounds); then it writes a one-line changelog
entry. Both tools are @khonsu.tool(replay="rerun"): on replay they run again for their
effect on the workspace, so the files and test results are real every time.
"""

import json
import os
import subprocess
import sys

import khonsu
from openai import OpenAI

MODEL = os.environ.get("MODEL", "qwen2.5:3b")
TASK = open("task.txt").read()
SUMMARY_PROMPT = os.environ.get("SUMMARY_PROMPT", "Summarize what you changed in one sentence for the changelog.")


@khonsu.tool(replay="rerun")
def write_file(path: str, content: str) -> str:
    if path not in ("solution.py",):
        return "error: you may only write solution.py"
    with open(path, "w") as f:
        f.write(content)
    return f"wrote {len(content)} bytes to {path}"


@khonsu.tool(replay="rerun")
def run_tests() -> str:
    r = subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", "test_solution.py"],
                       capture_output=True, text=True, timeout=60)
    out = (r.stdout + r.stderr).strip().splitlines()
    return ("PASS" if r.returncode == 0 else "FAIL") + "\n" + "\n".join(out[-25:])


TOOLS = [
    {"type": "function", "function": {"name": "write_file", "description": "Write the complete contents of solution.py",
     "parameters": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}},
                    "required": ["path", "content"]}}},
    {"type": "function", "function": {"name": "run_tests", "description": "Run the test suite for solution.py",
     "parameters": {"type": "object", "properties": {}}}},
]
IMPL = {"write_file": write_file, "run_tests": run_tests}

client = OpenAI(max_retries=0, timeout=300)
messages = [
    {"role": "system", "content": "You are a careful Python developer. Write solution.py with the write_file tool, "
                                  "then call run_tests. If tests fail, fix the code and run them again."},
    {"role": "user", "content": TASK},
]
passed = False
for round_ in range(5):
    r = client.chat.completions.create(model=MODEL, messages=messages, tools=TOOLS, temperature=0.7)
    msg = r.choices[0].message
    messages.append(msg.model_dump(exclude_none=True))
    if not msg.tool_calls:
        messages.append({"role": "user", "content": "Use the tools: write solution.py, then run_tests."})
        continue
    for call in msg.tool_calls:
        try:
            args = json.loads(call.function.arguments or "{}")
        except json.JSONDecodeError:
            args = {}
        # small models sometimes echo the schema: {"content": {"content": "...", "type": "string"}}
        args = {k: (v.get(k, v.get("value", v)) if isinstance(v, dict) else v) for k, v in args.items()}
        if isinstance(args.get("path"), str):
            args["path"] = os.path.basename(args["path"])
        fn = IMPL.get(call.function.name)
        try:
            out = fn(**args) if fn else f"error: unknown tool {call.function.name}"
        except TypeError as e:
            out = f"error: bad arguments: {e}"
        messages.append({"role": "tool", "tool_call_id": call.id, "content": out})
        if call.function.name == "run_tests" and out.startswith("PASS"):
            passed = True
    if passed:
        break

summary = client.chat.completions.create(model=MODEL, temperature=0.7,
                                         messages=messages + [{"role": "user", "content": SUMMARY_PROMPT}])
code = open("solution.py").read() if os.path.exists("solution.py") else ""
print(json.dumps({"passed": passed, "rounds": round_ + 1, "code_sha": __import__("hashlib").sha256(code.encode()).hexdigest()[:16],
                  "summary": summary.choices[0].message.content.strip()[:200]}))
