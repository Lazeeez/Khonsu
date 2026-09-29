"""The real-model study: what Khonsu changes for a real agent on a local model.

For each task:
  live   record one run with khonsu (workspace + tools), then run it live twice more;
  replay replay the recording 3x with the model *stopped* (no network at all);
  edit   change only the final summary prompt and replay-then-live.
Writes study/results.json and prints a summary table.

    python study/run_study.py [path/to/khonsu]     (needs Ollama at localhost:11434 with $MODEL pulled)
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from tasks import TASKS  # noqa: E402

KHONSU = str(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/native/khonsu").resolve())
PY = sys.executable
MODEL = os.environ.get("MODEL", "qwen2.5:3b")
OLLAMA = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434/v1")
TMP = Path(tempfile.mkdtemp(prefix="khonsu-study-"))
ENV = {**os.environ, "MODEL": MODEL, "PYTHONPATH": str(ROOT / "sdk/python/src"), "OPENAI_API_KEY": "ollama"}


def workspace(name, task_name=None):
    ws = TMP / name
    shutil.rmtree(ws, ignore_errors=True)
    (ws / ".git").mkdir(parents=True)
    task, tests = TASKS[task_name or name]
    (ws / "task.txt").write_text(task)
    (ws / "test_solution.py").write_text(tests)
    return ws


def outcome(stdout):
    lines = [l for l in stdout.splitlines() if l.startswith("{")]
    return json.loads(lines[-1]) if lines else {"passed": False, "error": "no result"}


def timed(args, cwd, env=ENV):
    t = time.perf_counter()
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True, env=env, timeout=1800)
    return r, time.perf_counter() - t


def calls_and_tokens(path):
    r = subprocess.run([KHONSU, "show", str(path), "--json"], capture_output=True, text=True)
    d = json.loads(r.stdout)
    model_calls = [e for e in d["exchanges"] if not e["tool"]]
    tok = sum(e["prompt_tokens"] + e["completion_tokens"] for e in model_calls)
    return len(model_calls), len(d["exchanges"]) - len(model_calls), tok


def ollama(action):
    if action == "stop":
        subprocess.run(["pkill", "-x", "ollama"], capture_output=True)
        time.sleep(1)
    else:
        subprocess.Popen(["ollama", "serve"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        for _ in range(60):
            if subprocess.run(["curl", "-sf", OLLAMA.replace("/v1", "/api/tags")], capture_output=True).returncode == 0:
                return
            time.sleep(1)


results = {"model": MODEL, "tasks": {}}
agent = [PY, str(HERE / "agent.py")]
for name in TASKS:
    print(f"== {name}", flush=True)
    t = {}
    # live: one recorded run + two more live runs of the identical task
    ws = workspace(name)
    rec = TMP / f"{name}.khn"
    r, secs = timed([KHONSU, "record", "-q", "-o", str(rec), "--upstream", OLLAMA, "--", *agent], ws)
    t["recorded"] = {**outcome(r.stdout), "seconds": round(secs, 2)}
    t["model_calls"], t["tool_calls"], t["tokens"] = calls_and_tokens(rec)
    lives = []
    for i in range(2):
        ws2 = workspace(f"{name}-live{i}", name)
        r2, s2 = timed([*agent], ws2, {**ENV, "OPENAI_BASE_URL": OLLAMA})
        lives.append({**outcome(r2.stdout), "seconds": round(s2, 2)})
    t["live_reruns"] = lives
    results["tasks"][name] = t
    print(json.dumps(t), flush=True)

# replay with the model server stopped: no network path to a model exists at all
ollama("stop")
for name in TASKS:
    t = results["tasks"][name]
    reps = []
    for i in range(3):
        ws = workspace(name)  # same path as the recording: tool output (pytest) embeds paths
        r, secs = timed([KHONSU, "replay", str(TMP / f"{name}.khn"), "-q", "--", *agent], ws)
        reps.append({**outcome(r.stdout), "seconds": round(secs, 3), "exit": r.returncode})
    t["replays"] = reps
ollama("start")

# the iteration experiment: edit only the final summary prompt
for name in TASKS:
    t = results["tasks"][name]
    ws = workspace(name)
    fork = TMP / f"{name}-edit.khn"
    env = {**ENV, "SUMMARY_PROMPT": "Write a one-sentence changelog entry in the imperative mood."}
    r, secs = timed([KHONSU, "replay", str(TMP / f"{name}.khn"), "--on-miss", "live", "--upstream", OLLAMA,
                     "-o", str(fork), "-q", "--", *agent], ws, env)
    d = json.loads(subprocess.run([KHONSU, "show", str(fork), "--json"], capture_output=True, text=True).stdout)
    model = [e for e in d["exchanges"] if not e["tool"]]
    reused = [e for e in model if e.get("from_recording")]
    t["edit"] = {"model_calls": len(model), "reused_from_recording": len(reused),
                 "tokens_total": sum(e["prompt_tokens"] + e["completion_tokens"] for e in model),
                 "tokens_reused": sum(e["prompt_tokens"] + e["completion_tokens"] for e in reused),
                 "seconds": round(secs, 2), **outcome(r.stdout)}

# ── aggregate ────────────────────────────────────────────────────────────────────────────────
T = results["tasks"].values()
n = len(results["tasks"])
same = lambda a, b: a.get("code_sha") == b.get("code_sha") and a.get("summary") == b.get("summary")
agg = {
    "tasks": n,
    "live_runs_differing_from_recording": sum(1 for t in T for l in t["live_reruns"] if not same(l, t["recorded"])),
    "live_runs_total": 2 * n,
    "pass_outcome_flips_between_live_runs": sum(1 for t in T if len({t["recorded"]["passed"], *(l["passed"] for l in t["live_reruns"])}) > 1),
    "replays_identical_to_recording": sum(1 for t in T for r in t["replays"] if same(r, t["recorded"]) and r["passed"] == t["recorded"]["passed"]),
    "replays_total": 3 * n,
    "mean_live_seconds": round(sum(t["recorded"]["seconds"] for t in T) / n, 2),
    "mean_replay_seconds": round(sum(r["seconds"] for t in T for r in t["replays"]) / (3 * n), 3),
    "edit_model_calls": sum(t["edit"]["model_calls"] for t in T),
    "edit_calls_reused": sum(t["edit"]["reused_from_recording"] for t in T),
    "edit_tokens_total": sum(t["edit"]["tokens_total"] for t in T),
    "edit_tokens_reused": sum(t["edit"]["tokens_reused"] for t in T),
    "recorded_pass_rate": sum(1 for t in T if t["recorded"]["passed"]) / n,
}
results["aggregate"] = agg
out = HERE / "results.json"
out.write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(agg, indent=2))
