"""Records the viewer's example runs with a real model (Ollama, qwen2.5:3b by default).

    python scripts/make_samples.py [path/to/khonsu]      (needs: ollama serving the model; python with openai)

Each example runs in a neutral folder (/tmp/khonsu-demo/...) as `python agent.py`, so no
personal paths end up in the published recordings. Every recording is a genuine run; for
the coding agent the script keeps recording until a run shows the whole story (a failing
test, a fix, passing tests), then records a second live run of the same task and a fork
from the first failed test.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KHONSU = str(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/native/khonsu").resolve())
OUT = ROOT / "web/public/samples"
DEMO = Path("/tmp/khonsu-demo")
UPSTREAM = os.environ.get("KHONSU_UPSTREAM", "http://127.0.0.1:11434/v1")
ENV = {**os.environ, "PYTHONPATH": str(ROOT / "sdk/python/src"), "OPENAI_API_KEY": "ollama",
       "MODEL": os.environ.get("MODEL", "qwen2.5:3b")}


def fresh(name: str, src: str) -> Path:
    ws = DEMO / name
    shutil.rmtree(ws, ignore_errors=True)
    shutil.copytree(ROOT / "samples" / src, ws)
    return ws


def khonsu(*args, cwd: Path):
    r = subprocess.run([KHONSU, *args], cwd=cwd, env=ENV, capture_output=True, text=True, timeout=1800)
    return r


def show(path: Path) -> dict:
    return json.loads(subprocess.run([KHONSU, "show", str(path), "--json"], capture_output=True, text=True).stdout)


def outcome(stdout: str) -> dict:
    lines = [l for l in stdout.splitlines() if l.startswith("{")]
    return json.loads(lines[-1]) if lines else {}


def tools_of(d: dict) -> list[str]:
    return [e["model"].removeprefix("tool __main__.") for e in d["exchanges"] if e["tool"]]


DEMO.mkdir(parents=True, exist_ok=True)
OUT.mkdir(parents=True, exist_ok=True)
for old in OUT.glob("*.khn"):
    old.unlink()

# ── 1. support agent: keep a run that looks up, checks the policy, refunds and emails ─────────
for attempt in range(1, 9):
    ws = fresh("support", "support")
    (DEMO / "outbox.log").unlink(missing_ok=True)
    rec = DEMO / "support.khn"
    rec.unlink(missing_ok=True)
    r = khonsu("record", "-q", "-o", str(rec), "--upstream", UPSTREAM, "-w", ".", "--", "python", "agent.py", cwd=ws)
    t = tools_of(show(rec)) if rec.exists() else []
    print(f"support attempt {attempt}: exit {r.returncode}, tools {t}", flush=True)
    if r.returncode == 0 and {"lookup_order", "get_refund_policy", "issue_refund", "send_email"} <= set(t):
        shutil.copy(rec, OUT / "support-agent.khn")
        break
else:
    sys.exit("no support run used all four tools; try again or use a larger model")

# ── 2. coding agent: keep a run that fails a test, fixes it, and passes ──────────────────────
first_fail = None
for attempt in range(1, 13):
    ws = fresh("coding", "coding")
    rec = DEMO / "coding.khn"
    rec.unlink(missing_ok=True)
    r = khonsu("record", "-q", "-o", str(rec), "--upstream", UPSTREAM, "-w", ".", "--", "python", "agent.py", cwd=ws)
    res = outcome(r.stdout)
    d = show(rec) if rec.exists() else {"exchanges": []}
    tests = [e for e in d["exchanges"] if e["tool"] and e["model"].endswith("run_tests")]
    fails = [e for e in tests if str(e.get("output", "")).startswith("FAIL")]
    print(f"coding attempt {attempt}: passed={res.get('passed')} test runs={len(tests)} failing={len(fails)}", flush=True)
    if res.get("passed") and fails and tests[-1] not in fails:
        shutil.copy(rec, OUT / "coding-agent.khn")
        first_fail = fails[0]["seq"]
        break
else:
    sys.exit("no coding run showed fail -> fix -> pass; try again")

# ── 3. the same task, run again live: usually a different path ─────────────────────────────
for attempt in range(1, 7):
    ws = fresh("coding", "coding")
    rec = DEMO / "coding-again.khn"
    rec.unlink(missing_ok=True)
    r = khonsu("record", "-q", "-o", str(rec), "--upstream", UPSTREAM, "-w", ".", "--", "python", "agent.py", cwd=ws)
    first = show(OUT / "coding-agent.khn")["exchanges"][0]
    again = show(rec)["exchanges"][0]
    print(f"rerun attempt {attempt}: first answer differs={first.get('output') != again.get('output') or first.get('tool_calls') != again.get('tool_calls')}", flush=True)
    if r.returncode == 0 and len(show(rec)["exchanges"]) > 2:
        shutil.copy(rec, OUT / "coding-agent-again.khn")
        break

# ── 4. a fork from the moment after the first failing test ─────────────────────────────────
fork_at = first_fail + 1  # the model call that reads the failure and tries a fix
fork_dir = DEMO / "coding-fork"
shutil.rmtree(fork_dir, ignore_errors=True)
rec = DEMO / "coding-fork.khn"
rec.unlink(missing_ok=True)
r = khonsu("fork", f"{OUT / 'coding-agent.khn'}@{fork_at}", "--dir", str(fork_dir), "-o", str(rec),
           "--upstream", UPSTREAM, "--", "python", "agent.py", cwd=DEMO)
print(f"fork at #{fork_at}: exit {r.returncode}", r.stderr[-300:], flush=True)
shutil.copy(rec, OUT / "coding-agent-fork.khn")

print(json.dumps({"coding_first_fail": first_fail, "fork_at": fork_at}))
