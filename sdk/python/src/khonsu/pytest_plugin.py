"""pytest plugin: deterministic, offline, free agent tests backed by Khonsu recordings.

    import pytest

    @pytest.mark.khonsu("recordings/refund_flow.khn")
    def test_refund_flow():
        result = run_my_agent("I want a refund for order 1234")
        assert result.action == "refund"

- Normal runs replay the recording: no network, no tokens, same answer every time.
  If the agent's requests changed (prompt edits, new tool results), the test fails
  with Khonsu's divergence report: which call, which JSON field, old vs new value.
- ``pytest --khonsu-record`` re-records against a live model (``--khonsu-upstream URL``
  or $KHONSU_UPSTREAM / $OPENAI_BASE_URL) and overwrites the recording.
- Relative recording paths are resolved against the test file's directory.

Needs the ``khonsu`` binary: on PATH, in $KHONSU_BIN, or ``--khonsu-bin``.
Model clients must be created inside the test (they read OPENAI_BASE_URL then).
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import urllib.request
from pathlib import Path

import pytest

MARKER = "khonsu"


def pytest_addoption(parser):
    g = parser.getgroup("khonsu", "Khonsu agent recordings")
    g.addoption("--khonsu-record", action="store_true", default=False,
                help="re-record Khonsu recordings against a live model instead of replaying them")
    g.addoption("--khonsu-bin", default=None, help="path to the khonsu binary")
    g.addoption("--khonsu-upstream", default=None, help="model API base URL used when recording")


def pytest_configure(config):
    config.addinivalue_line(
        "markers", f"{MARKER}(path, on_miss='fail'): run the test against a Khonsu recording (replay) "
                   "or record it with --khonsu-record")


def pytest_collection_modifyitems(items):
    for item in items:  # marked tests get the fixture without having to ask for it
        if item.get_closest_marker(MARKER) and "khonsu_run" not in item.fixturenames:
            item.fixturenames.append("khonsu_run")


class KhonsuServer:
    """A `khonsu serve` process bound to one recording."""

    def __init__(self, binary: str, recording: Path, record: bool, upstream: str | None, on_miss: str):
        args = [binary, "serve", "--record" if record else "--replay", str(recording), "-q"]
        if not record:
            args += ["--on-miss", on_miss]
        if upstream:
            args += ["--upstream", upstream]
        self.record = record
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.env: dict[str, str] = {}
        while len(self.env) < 2:  # "export OPENAI_BASE_URL=…" and "export KHONSU_TOOL_URL=…"
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError(f"khonsu serve failed to start: {self.proc.stderr.read()}")
            if line.startswith("export ") and "=" in line:
                k, v = line[len("export "):].strip().split("=", 1)
                self.env[k] = v
        self.origin = self.env["KHONSU_TOOL_URL"].split("/khonsu/")[0]

    def status(self) -> dict:
        with urllib.request.urlopen(self.origin + "/khonsu/v1/status", timeout=10) as r:
            return json.loads(r.read())

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)  # lets khonsu write RunEnd and fsync
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def _binary(config) -> str:
    b = config.getoption("--khonsu-bin") or os.environ.get("KHONSU_BIN") or shutil.which("khonsu")
    if not b:
        pytest.fail("khonsu binary not found: put it on PATH, set $KHONSU_BIN, or pass --khonsu-bin", pytrace=False)
    return b


def _report(status: dict, recording: Path) -> str:
    lines = [f"agent behaviour diverged from {recording} "
             f"({status['replayed']} calls replayed, {len(status['divergences'])} divergent):"]
    for d in status["divergences"]:
        lines.append(f"  request #{d['seq']} {d['path']}: expected recorded call #{d['expected_seq']}, "
                     f"first difference at `{d['field']}`")
        if d.get("expected") or d.get("actual"):
            lines.append(f"    recorded: {d['expected']}")
            lines.append(f"    now:      {d['actual']}")
    lines.append("If the change is intended, re-record with: pytest --khonsu-record")
    return "\n".join(lines)


@pytest.fixture
def khonsu_run(request, monkeypatch):
    marker = request.node.get_closest_marker(MARKER)
    if marker is None or not marker.args:
        pytest.fail("use @pytest.mark.khonsu('path/to/recording.khn') with the khonsu_run fixture", pytrace=False)
    recording = Path(marker.args[0])
    if not recording.is_absolute():
        recording = Path(request.node.fspath).parent / recording
    record = request.config.getoption("--khonsu-record")
    if not record and not recording.exists():
        pytest.fail(f"recording {recording} does not exist; create it with: pytest --khonsu-record", pytrace=False)
    if record:
        recording.parent.mkdir(parents=True, exist_ok=True)
        recording.unlink(missing_ok=True)

    server = KhonsuServer(_binary(request.config), recording, record,
                          request.config.getoption("--khonsu-upstream"), marker.kwargs.get("on_miss", "fail"))
    for k, v in server.env.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("OPENAI_API_BASE", server.env["OPENAI_BASE_URL"])
    monkeypatch.setenv("KHONSU_MODE", "record" if record else "replay")
    if not os.environ.get("OPENAI_API_KEY"):
        monkeypatch.setenv("OPENAI_API_KEY", "khonsu-no-key")
    try:
        yield server
        status = server.status()
    finally:
        server.stop()
    if not record and status["divergences"]:
        pytest.fail(_report(status, recording), pytrace=False)
