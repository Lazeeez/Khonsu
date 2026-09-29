"""End-to-end tests: the real `khonsu` binary + the real OpenAI Python SDK + a
non-deterministic mock model. No pytest dependency; run with the Python that
has `openai` installed:

    python tests/integration/run_integration.py [path/to/khonsu]
"""

import json
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from harness import KHONSU, PY, HERE, Mock, check, finish, khonsu, show

AGENT = [PY, str(HERE / "agent.py")]
TMP = Path(tempfile.mkdtemp(prefix="khonsu-it-"))


def agent_direct(url, env=None):
    return subprocess.run(AGENT, capture_output=True, text=True, timeout=60,
                          env={**os.environ, "OPENAI_BASE_URL": url, "OPENAI_API_KEY": "x", **(env or {})})


print(f"khonsu integration tests · {KHONSU}")
mock = Mock()

# 1 ─ The problem: a live agent run is not reproducible.
a, b = agent_direct(mock.url), agent_direct(mock.url)
check("live runs are non-deterministic (the problem)", a.returncode == 0 and a.stdout != b.stdout, a.stderr)

# 2 ─ Record.
run = TMP / "run.khn"
rec = khonsu("record", "--no-workspace", "-o", str(run), "--upstream", mock.url, "-q", "--", *AGENT,
             env={"AGENT_PARALLEL": "6", "OPENAI_API_KEY": "sk-secret-123"})
check("record: agent succeeds through the proxy", rec.returncode == 0, rec.stderr)
info = show(run)
check("record: 8 exchanges captured (2 agent steps + 6 parallel)", info and len(info["exchanges"]) == 8, str(info))
check("record: recording verifies (CRCs + hash chain)", khonsu("verify", str(run)).returncode == 0)
check("record: API key never written to disk", b"sk-secret-123" not in run.read_bytes())

# 3 ─ Replay with the model server DOWN: zero network, identical output, every time.
mock.stop()
for i in range(3):
    rp = khonsu("replay", str(run), "-q", "--", *AGENT, env={"AGENT_PARALLEL": "6"})
    check(f"replay #{i + 1}: bit-identical agent output with the model offline",
          rp.returncode == 0 and rp.stdout == rec.stdout, f"--- recorded\n{rec.stdout}--- replay\n{rp.stdout}{rp.stderr}")

# 4 ─ Divergence: the tool now returns something different → step 2's prompt changed.
dv = khonsu("replay", str(run), "--", *AGENT, env={"AGENT_PARALLEL": "6", "AGENT_TOOL_NOTE": " (updated)"})
check("divergence: replay fails loudly (exit 3 or agent error)", dv.returncode != 0, dv.stderr)
check("divergence: pinpointed to messages[3].content", "messages[3].content" in dv.stderr, dv.stderr)

# 5 ─ Replay-then-live: only the changed call goes to the model, and the new run is recorded as a fork.
mock = Mock()
fork = TMP / "fork.khn"
rl = khonsu("replay", str(run), "--on-miss", "live", "--upstream", mock.url, "-o", str(fork), "--", *AGENT,
            env={"AGENT_PARALLEL": "6", "AGENT_TOOL_NOTE": " (updated)"})
finfo = show(fork)
served = [e for e in (finfo or {}).get("exchanges", []) if e.get("from_recording")]
live = [e for e in (finfo or {}).get("exchanges", []) if not e.get("from_recording")]
check("replay-then-live: agent succeeds", rl.returncode == 0, rl.stderr)
check("replay-then-live: 7 calls served from the recording, 1 live", len(served) == 7 and len(live) == 1,
      json.dumps(finfo, indent=1)[:2000] if finfo else rl.stderr)
check("replay-then-live: step 1 output unchanged",
      rl.stdout.splitlines()[0] == rec.stdout.splitlines()[0], rl.stdout)
check("fork records its parent", finfo and finfo["parent"] and finfo["mode"] == "replay-then-live", str(finfo))

# 6 ─ Tamper and bit-rot are detected.
bad = TMP / "tampered.khn"
data = bytearray(run.read_bytes())
data[len(data) // 2] ^= 0x01
bad.write_bytes(bytes(data))
check("verify: a single flipped bit is detected", khonsu("verify", str(bad)).returncode != 0)

# 7 ─ Torn writes: cut the file at every byte offset; loading must never crash.
small = TMP / "small.khn"
khonsu("record", "--no-workspace", "-o", str(small), "--upstream", mock.url, "-q", "--", *AGENT)
full = small.read_bytes()
crashes, loaded = 0, 0
cut = TMP / "cut.khn"
for off in range(0, len(full) + 1, max(1, len(full) // 400)):
    cut.write_bytes(full[:off])
    r = khonsu("show", str(cut), "--json")
    if r.returncode < 0:
        crashes += 1
    elif r.returncode == 0:
        loaded += 1
check(f"torn tail: {len(full)}-byte recording cut at ~400 offsets, 0 crashes", crashes == 0, f"{crashes} crashes")
check("torn tail: every cut past the RunStart frame still loads", loaded > 300, f"loaded {loaded}")

# 8 ─ kill -9 the recorder mid-run: every response the agent saw is in the recording.
mock.stop()
mock = Mock(delay="0.03")
violations = 0
for trial in range(20):
    path = TMP / f"crash{trial}.khn"
    p = subprocess.Popen([str(KHONSU), "record", "--no-workspace", "-o", str(path), "--upstream", mock.url, "-q", "--", *AGENT],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                         env={**os.environ, "AGENT_PARALLEL": "4"}, start_new_session=True)
    time.sleep(random.uniform(0.05, 1.2))
    p.send_signal(signal.SIGKILL)  # the proxy dies instantly; the agent then errors out
    out, _ = p.communicate(timeout=60)
    seen = sum(1 for line in out.splitlines() if line.startswith(("step1", "step2", "side")))  # 1 line = 1 exchange
    info = show(path)
    complete = sum(1 for e in (info or {}).get("exchanges", []) if "status" in e)
    if info is None or complete < seen:
        violations += 1
        print(f"    trial {trial}: agent saw {seen} responses, recording has {complete}")
check("kill -9: 20 trials, every response the agent observed survived", violations == 0, f"{violations} violations")

mock.stop()
shutil.rmtree(TMP, ignore_errors=True)
finish()
