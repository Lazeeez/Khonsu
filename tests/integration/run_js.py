"""The JavaScript/TypeScript SDK end to end: record a Node agent, replay it offline.

    python tests/integration/run_js.py [path/to/khonsu]   (needs node >= 18 on PATH)
"""

import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from harness import HERE, Mock, check, finish, khonsu, show

TMP = Path(tempfile.mkdtemp(prefix="khonsu-js-"))
SIDE = TMP / "side_effects.log"
AGENT = ["node", str(HERE / "js_agent.mjs")]
ENV = {"SIDE_EFFECTS": str(SIDE)}


def lines(p):
    return p.read_text().splitlines() if p.exists() else []


def result(r):
    out = [l for l in r.stdout.splitlines() if l.startswith("{")]
    return json.loads(out[-1]) if out else {}


print("khonsu JavaScript SDK tests")
node = shutil.which("node")
check("node is available", node is not None, "install Node.js >= 18")
if not node:
    finish()

# Outside Khonsu the wrappers just call through (no proxy needed for the tools).
mock = Mock()
work = TMP / "plain"
work.mkdir()
plain = subprocess.run(AGENT, cwd=work, capture_output=True, text=True, timeout=60,
                       env={**os.environ, **ENV, "OPENAI_BASE_URL": mock.url, "OPENAI_API_KEY": "k"})
check("plain run: tools work without khonsu", plain.returncode == 0 and result(plain).get("wrote", 0) > 0, plain.stderr)
SIDE.unlink(missing_ok=True)

run = TMP / "run.khn"
rec_dir = TMP / "rec"
rec_dir.mkdir()
rec = khonsu("record", "-o", str(run), "--upstream", mock.url, "--", *AGENT, env=ENV, cwd=rec_dir)
R = result(rec)
check("record: agent succeeds", rec.returncode == 0 and R, rec.stderr + rec.stdout)
d = show(run)
tools = [e for e in (d or {}).get("exchanges", []) if e["tool"]]
check("record: 6 tool calls + 1 model call captured", d and len(tools) == 6 and len(d["exchanges"]) == 7,
      json.dumps([e["model"] for e in (d or {}).get("exchanges", [])]))
check("record: a thrown RangeError is recorded as an error outcome", R.get("errName") == "RangeError: boom", str(R))
check("record: a non-JSON result fails with TypeError and the call is still closed",
      R.get("badName") == "TypeError" and any("bigint" in e["model"] for e in tools), str(R))
check("record: the side effect happened once", lines(SIDE) == ["agent finished"], str(lines(SIDE)))

mock.stop()
rep_dir = TMP / "replay"
rep_dir.mkdir()
rp = khonsu("replay", str(run), "--", *AGENT, env=ENV, cwd=rep_dir)
P = result(rp)
check("replay (model offline): identical output", rp.returncode == 0 and P == R, f"{R}\n---\n{P}\n{rp.stderr}")
check("replay: the external side effect is not repeated", lines(SIDE) == ["agent finished"], str(lines(SIDE)))
check("replay: a rerun tool ran again for its effect", (rep_dir / "answer.md").exists())
check("replay: a rerun tool's changed report is flagged, the recorded one returned",
      "tool lint re-ran with a different result" in rp.stderr and P.get("report") == R.get("report"), rp.stderr)
check("replay: the recorded error is rethrown as the same type", P.get("errName") == "RangeError: boom", str(P))

div = khonsu("replay", str(run), "--", *AGENT, env={**ENV, "NOTIFY_MESSAGE": "something else"}, cwd=rep_dir)
check("replay: changed tool arguments raise KhonsuDivergence", result(div).get("divergence") == "KhonsuDivergence",
      div.stdout + div.stderr)
finish()
