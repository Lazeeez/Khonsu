"""Milestone 3 end-to-end tests: `khonsu diff` and the pytest plugin.

    python tests/integration/run_ci.py [path/to/khonsu]   (needs openai + pytest; SDK on PYTHONPATH)
"""

import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from harness import HERE, KHONSU, PY, ROOT, Mock, check, finish, khonsu

TMP = Path(tempfile.mkdtemp(prefix="khonsu-ci-"))
AGENT = [PY, str(HERE / "agent.py")]


def diff(a, b):
    r = khonsu("diff", str(a), str(b), "--json")
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else None), r


print(f"khonsu CI tests · {KHONSU}")
mock = Mock()

# ── khonsu diff ──────────────────────────────────────────────────────────────────────────────
run_a, run_b, replayed = TMP / "a.khn", TMP / "b.khn", TMP / "a-replay.khn"
for path in (run_a, run_b):
    khonsu("record", "--no-workspace", "-q", "-o", str(path), "--upstream", mock.url, "--", *AGENT)

code, d, raw = diff(run_a, run_b)
check("diff: two live runs differ (exit 1)", code == 1 and d and not d["identical"], raw.stdout + raw.stderr)
first = d["calls"][d["first_divergence"]] if d else {}
check("diff: first divergence is call #0: same request, different output (tool arguments sampled differently)",
      first.get("kind") == "same-request" and first.get("a_seq") == 0 and "get_weather(" in first.get("a", ""),
      json.dumps(d, indent=1)[:1500])
check("diff: the next call's request differs because it feeds that result back",
      d and d["calls"][1]["kind"] == "changed" and d["calls"][1]["field"].startswith("messages["), json.dumps(d, indent=1)[:1500])

khonsu("replay", str(run_a), "-q", "-o", str(replayed), "--", *AGENT)
code, d, raw = diff(run_a, replayed)
check("diff: a run and its replay are identical (exit 0)", code == 0 and d and d["identical"] and d["same"] == 2,
      raw.stdout + raw.stderr)

human = khonsu("diff", str(run_a), str(run_b))
check("diff: human output names the first divergence", "first divergence" in human.stdout, human.stdout)

# ── pytest plugin ─────────────────────────────────────────────────────────────────────────────
proj = TMP / "proj"
(proj / "tests").mkdir(parents=True)
(proj / "tests" / "test_agent.py").write_text('''
import os
import pytest
from openai import OpenAI

def ask(question):
    client = OpenAI(max_retries=0)   # created inside the test: picks up Khonsu's base URL
    r = client.chat.completions.create(model="mock-1", messages=[{"role": "user", "content": question}])
    return r.choices[0].message.content

@pytest.mark.khonsu("recordings/ask.khn")
def test_agent_answers():
    answer = ask(os.environ.get("QUESTION", "Who is Khonsu?"))
    with open(os.environ["ANSWER_LOG"], "a") as f:
        f.write(answer + "\\n")
    assert answer
''')
log = TMP / "answers.log"
env = {**os.environ, "PYTHONPATH": str(ROOT / "sdk/python/src"), "ANSWER_LOG": str(log)}


def pytest_run(*extra, env_extra=None):
    return subprocess.run([PY, "-m", "pytest", "-q", "-p", "khonsu.pytest_plugin", "--khonsu-bin", str(KHONSU),
                           "-p", "no:cacheprovider", *extra, "tests"],
                          cwd=proj, capture_output=True, text=True, timeout=120, env={**env, **(env_extra or {})})


rec = pytest_run("--khonsu-record", "--khonsu-upstream", mock.url)
check("pytest --khonsu-record: test passes against the live model and writes the recording",
      rec.returncode == 0 and (proj / "tests/recordings/ask.khn").exists(), rec.stdout + rec.stderr)

mock.stop()
r1 = pytest_run()
r2 = pytest_run()
answers = log.read_text().splitlines()
check("pytest replay: passes with the model offline", r1.returncode == 0 and r2.returncode == 0, r1.stdout + r1.stderr)
check("pytest replay: deterministic (recorded answer returned every time)",
      len(answers) == 3 and answers[0] == answers[1] == answers[2], str(answers))

changed = pytest_run(env_extra={"QUESTION": "Who is Thoth?"})
check("pytest replay: a prompt change fails the test with the divergence report",
      changed.returncode != 0 and "first difference at `messages[0].content`" in changed.stdout
      and "Who is Thoth?" in changed.stdout and "--khonsu-record" in changed.stdout, changed.stdout[-2500:])

shutil.rmtree(proj / "tests" / "recordings")
missing = pytest_run()
check("pytest: a missing recording fails with instructions", missing.returncode != 0 and "--khonsu-record" in missing.stdout,
      missing.stdout[-1500:])

shutil.rmtree(TMP, ignore_errors=True)
finish()
