"""Shared helpers for the integration suites (no pytest dependency)."""

import atexit
import json
import os
import socket
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
KHONSU = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/native/khonsu").resolve()
PY = sys.executable

passed = 0
failed = 0
_children = []  # background servers; killed on any exit so a failing suite can't hang its caller


def spawn(*args, **kwargs):
    p = subprocess.Popen(*args, **kwargs)
    _children.append(p)
    return p


@atexit.register
def _reap():
    for p in _children:
        if p.poll() is None:
            p.kill()
            p.wait()


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok   {name}")
    else:
        failed += 1
        print(f"  FAIL {name}\n{detail}")


def finish():
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Mock:
    """The non-deterministic OpenAI-compatible model server (mock_llm.py)."""

    def __init__(self, delay="0.002"):
        self.port = free_port()
        env = {**os.environ, "MOCK_DELAY": delay}
        self.p = spawn([PY, str(HERE / "mock_llm.py"), str(self.port)], stdout=subprocess.PIPE, env=env)
        assert self.p.stdout.readline().strip() == b"ready"
        self.url = f"http://127.0.0.1:{self.port}/v1"

    def stop(self):
        self.p.terminate()
        self.p.wait()


def khonsu(*args, env=None, timeout=60, cwd=None):
    return subprocess.run([str(KHONSU), *args], capture_output=True, text=True, timeout=timeout, cwd=cwd,
                          env={**os.environ, **(env or {})})


def show(path):
    r = khonsu("show", str(path), "--json")
    return json.loads(r.stdout) if r.returncode == 0 else None
