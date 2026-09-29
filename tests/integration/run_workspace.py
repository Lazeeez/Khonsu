"""Milestone 2 end-to-end tests: workspace snapshots, time travel, forks, @khonsu.tool.

    python tests/integration/run_workspace.py [path/to/khonsu]   (needs openai + the SDK importable)
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from harness import HERE, KHONSU, PY, ROOT, Mock, check, finish, khonsu, show
AGENT = [PY, str(HERE / "coder_agent.py")]
TMP = Path(tempfile.mkdtemp(prefix="khonsu-ws-"))
SIDE = TMP / "side_effects.log"
ENV = {"SIDE_EFFECTS": str(SIDE), "PYTHONPATH": str(ROOT / "sdk/python/src")}


def lines(p):
    return p.read_text().splitlines() if p.exists() else []


def workspace():
    ws = TMP / "ws"
    (ws / ".git").mkdir(parents=True)             # a git checkout: captured by default, .git itself ignored
    (ws / ".git" / "HEAD").write_text("ref: refs/heads/main\n")
    (ws / "task.txt").write_text("Write a haiku about the moon god Khonsu.\n")
    (ws / "src").mkdir()
    (ws / "src" / "lib.py").write_text("def f():\n    return 42\n" * 50)
    (ws / "big.bin").write_bytes(os.urandom(9 << 20))  # > 8 MiB: tracked, not stored
    # .gitignore semantics: dir-only, globs, negation, nested files, anchoring.
    (ws / ".gitignore").write_text("build/\n*.log\n!keep.log\n/root-only.txt\n")
    (ws / "build").mkdir()
    (ws / "build" / "artifact.o").write_bytes(b"\0" * 1000)
    (ws / "debug.log").write_text("noise\n")
    (ws / "keep.log").write_text("important\n")
    (ws / "root-only.txt").write_text("x\n")
    (ws / "src" / "root-only.txt").write_text("not anchored here\n")
    (ws / "src" / ".gitignore").write_text("generated.py\n")
    (ws / "src" / "generated.py").write_text("# generated\n")
    # Secrets are never captured, even without a .gitignore entry.
    (ws / ".env").write_text("OPENAI_API_KEY=sk-live-should-never-leak\n")
    (ws / "deploy.pem").write_text("-----BEGIN PRIVATE KEY-----\n")
    return ws


def run_in(cwd, *args, env=None):
    return subprocess.run([str(KHONSU), *args], cwd=cwd, capture_output=True, text=True, timeout=120,
                          env={**os.environ, **ENV, **(env or {})})


print(f"khonsu workspace tests · {KHONSU}")
mock = Mock()
ws = workspace()
run = TMP / "run.khn"

# 1 ─ Record: snapshots at every call, tools recorded, external side effect happens once.
rec = run_in(ws, "record", "-o", str(run), "--upstream", mock.url, "--", *AGENT)
check("record: agent succeeds", rec.returncode == 0, rec.stderr)
info = show(run)
calls = info["exchanges"] if info else []
tools = [e for e in calls if e["tool"]]
check("record: 2 model calls + 6 tool calls captured", len(calls) == 8 and len(tools) == 6,
      json.dumps(calls, indent=1)[:1500])
check("record: a snapshot before every call, plus start and end", info and info["snapshots"] == len(calls) + 2, str(info))
check("record: side effect happened once", lines(SIDE) == ["agent finished"], str(lines(SIDE)))
listing = khonsu("files", f"{run}@start").stdout
names = {line.split()[-1] for line in listing.splitlines() if line.strip()}
check("record: .git was not captured", not any(n.startswith(".git/") for n in names), listing)
check("ignore: .gitignore honoured (dir-only, glob, negation, nested, anchored)",
      "build/artifact.o" not in names and "debug.log" not in names and "keep.log" in names
      and "root-only.txt" not in names and "src/root-only.txt" in names and "src/generated.py" not in names
      and "src/lib.py" in names, listing)
check("ignore: secret files never captured (.env, *.pem)", ".env" not in names and "deploy.pem" not in names
      and b"sk-live-should-never-leak" not in run.read_bytes(), listing)
ws_changes = [c for e in calls for c in e["workspace_changes"]]
check("record: per-call workspace changes attributed", "A out/answer.md" in ws_changes and "A out/review.md" in ws_changes,
      str(ws_changes))
check("verify: every snapshot restorable from the file alone", khonsu("verify", str(run)).returncode == 0,
      khonsu("verify", str(run)).stdout)

# 2 ─ Time travel: files at any point in the run.
answer_line = rec.stdout.splitlines()[0].removeprefix("answer ")
end_answer = khonsu("cat", f"{run}@end", "out/answer.md")
check("cat @end: file as the agent left it", end_answer.returncode == 0 and answer_line in end_answer.stdout, end_answer.stderr)
first_write = next(e["seq"] for e in calls if e["tool"] and "out/answer.md" in e["input"])
check(f"cat @{first_write}: file did not exist yet before the call that wrote it",
      khonsu("cat", f"{run}@{first_write}", "out/answer.md").returncode == 1)
check(f"cat @{first_write + 1}: …and exists right after",
      khonsu("cat", f"{run}@{first_write + 1}", "out/answer.md").returncode == 0)
start_dir = TMP / "at-start"
co = khonsu("checkout", f"{run}@start", str(start_dir))
check("checkout @start: original workspace restored",
      co.returncode == 0 and (start_dir / "task.txt").read_text() == (ws / "task.txt").read_text()
      and (start_dir / "src/lib.py").read_bytes() == (ws / "src/lib.py").read_bytes()
      and not (start_dir / "out").exists(), co.stderr)
check("checkout: files over 8 MiB are tracked but not stored (and reported)",
      not (start_dir / "big.bin").exists() and "not stored" in co.stderr, co.stderr)

# 3 ─ Replay from a pristine copy of the start: identical output, no external side effects, workspace verified.
mock.stop()
replay_dir = TMP / "replay"
khonsu("checkout", f"{run}@start", str(replay_dir))
shutil.copy(ws / "big.bin", replay_dir / "big.bin")
rp = run_in(replay_dir, "replay", str(run), "-w", str(replay_dir), "--", *AGENT)
check("replay: identical output, including the recorded clock", rp.returncode == 0 and rp.stdout == rec.stdout,
      f"{rec.stdout}\n---\n{rp.stdout}\n{rp.stderr}")
check("replay: external side effect NOT repeated", lines(SIDE) == ["agent finished"], str(lines(SIDE)))
check("replay: a rerun tool's differing report is flagged and the recorded one returned",
      "tool __main__.lint re-ran with a different result" in rp.stderr, rp.stderr)
check("replay: workspace identical to the recording at every call", "workspace identical" in rp.stderr, rp.stderr)

# 4 ─ A stray file in the workspace is reported, with its path.
stray_dir = TMP / "stray"
khonsu("checkout", f"{run}@start", str(stray_dir))
shutil.copy(ws / "big.bin", stray_dir / "big.bin")
(stray_dir / "scratch.txt").write_text("left over from another run\n")
st = run_in(stray_dir, "replay", str(run), "-w", str(stray_dir), "--", *AGENT)
check("replay: workspace drift detected and named (+scratch.txt)",
      st.returncode == 0 and "+scratch.txt" in st.stderr and "mismatches" in st.stderr, st.stderr)

# 5 ─ Fork at the review call: earlier calls replay, the review goes live, the new run is recorded.
mock = Mock()
review_seq = [e["seq"] for e in calls if not e["tool"]][1]
fork_dir, fork_rec = TMP / "fork", TMP / "fork.khn"
fk = run_in(TMP, "fork", f"{run}@{review_seq}", "--dir", str(fork_dir), "-o", str(fork_rec),
            "--upstream", mock.url, "--", *AGENT)
fout = fk.stdout.splitlines()
check("fork: agent succeeds in the restored workspace", fk.returncode == 0, fk.stderr + fk.stdout)
check("fork: everything before the fork point is replayed (same answer, same clock)",
      fout[0] == rec.stdout.splitlines()[0] and fout[2] == rec.stdout.splitlines()[2], fk.stdout)
check("fork: the review is regenerated live", fout[1] != rec.stdout.splitlines()[1], fk.stdout)
check("fork: side effects after the fork point run for real", lines(SIDE) == ["agent finished"] * 2, str(lines(SIDE)))
finfo = show(fork_rec)
check("fork: recorded with its parent, no divergences reported",
      finfo and finfo["mode"] == "fork" and finfo["parent"] and "0 divergences" in fk.stderr, fk.stderr)
check("fork: the forked recording has its own snapshots", finfo and finfo["snapshots"] >= len(calls), str(finfo))

mock.stop()
shutil.rmtree(TMP, ignore_errors=True)
finish()
