"""Shared helpers for the comparative benchmarks (Khonsu vs vcrpy vs LangGraph)."""

import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KHONSU = os.environ.get("KHONSU_BIN", str(ROOT / "build/native/khonsu"))
MOCK = ROOT / "tests/integration/mock_llm.py"
PY = sys.executable


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Proc:
    """A background process that is always cleaned up."""

    def __init__(self, args, env=None, ready=None):
        self.p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                  env={**os.environ, **(env or {})})
        self.lines = []
        if ready:
            while True:
                line = self.p.stdout.readline()
                if not line:
                    raise RuntimeError(f"{args[0]} exited: {self.p.stderr.read()}")
                self.lines.append(line.strip())
                if ready(line):
                    break

    def stop(self, sig=signal.SIGINT):
        if self.p.poll() is None:
            self.p.send_signal(sig)
            try:
                self.p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.p.kill()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.stop()


def mock_server(delay="0"):
    port = free_port()
    p = Proc([PY, str(MOCK), str(port)], env={"MOCK_DELAY": delay}, ready=lambda l: l.strip() == "ready")
    return p, f"http://127.0.0.1:{port}/v1"


def khonsu_serve(*args):
    """Starts `khonsu serve …` and returns (proc, base_url)."""
    p = Proc([KHONSU, "serve", "-q", *args], ready=lambda l: l.startswith("export KHONSU_TOOL_URL"))
    base = next(l.split("=", 1)[1] for l in p.lines if l.startswith("export OPENAI_BASE_URL"))
    return p, base


def percentile(xs, q):
    xs = sorted(xs)
    if not xs:
        return 0.0
    k = (len(xs) - 1) * q
    f, c = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def summarize_ms(samples):
    return {"p50_ms": round(percentile(samples, 0.5) * 1e3, 3), "p99_ms": round(percentile(samples, 0.99) * 1e3, 3),
            "mean_ms": round(sum(samples) / max(len(samples), 1) * 1e3, 3), "n": len(samples)}


def now():
    return time.perf_counter()


def write_result(name, data):
    out = ROOT / "bench/results"
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{name}.json").write_text(json.dumps(data, indent=2) + "\n")
    print(json.dumps(data, indent=2))
