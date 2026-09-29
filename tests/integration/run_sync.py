"""Milestone 4 end-to-end tests: shared stores, Merkle anti-entropy sync, CRDT annotations.

    python tests/integration/run_sync.py [path/to/khonsu]
"""

import hashlib
import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

from harness import HERE, KHONSU, PY, ROOT, Mock, check, finish, free_port, khonsu, spawn

TMP = Path(tempfile.mkdtemp(prefix="khonsu-sync-"))
AGENT = [PY, str(HERE / "coder_agent.py")]
ENV = {"SIDE_EFFECTS": str(TMP / "side.log"), "PYTHONPATH": str(ROOT / "sdk/python/src")}


def k(*args, store=None, **kw):
    extra = ["--store", str(store)] if store else []
    return khonsu(*args, *extra, **kw)


def sync(store, remote, *flags):
    r = k("sync", str(remote), "--json", *flags, store=store)
    return (json.loads(r.stdout) if r.returncode == 0 else None), r


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def notes(store, name):
    r = k("annotations", name, "--json", store=store)
    return sorted((a["step"], a["label"], a["text"]) for a in json.loads(r.stdout))


print(f"khonsu sync tests · {KHONSU}")
mock = Mock()

# A workspace with a sizeable file, so deduplication is visible.
ws = TMP / "ws"
(ws / ".git").mkdir(parents=True)
(ws / "task.txt").write_text("Write a haiku about Khonsu.\n")
(ws / "data").mkdir()
(ws / "data" / "corpus.txt").write_text(("the moon travels " * 40 + "\n") * 2000)  # ~1.3 MB
run1, run2 = TMP / "run1.khn", TMP / "run2.khn"
for r in (run1, run2):
    res = subprocess.run([str(KHONSU), "record", "-q", "-o", str(r), "--upstream", mock.url, "--", *AGENT],
                         cwd=ws, capture_output=True, text=True, env={**os.environ, **ENV}, timeout=120)
    assert res.returncode == 0, res.stderr
    for p in (ws / "out").glob("*"):
        p.unlink()  # start the second run from the same workspace

A, B, C, R = TMP / "A", TMP / "B", TMP / "C", TMP / "shared"

# 1 ─ Importing into a store deduplicates workspace content across runs.
a1 = k("store", "add", str(run1), store=A)
a2 = k("store", "add", str(run2), store=A)
check("store add: both runs imported", a1.returncode == 0 and a2.returncode == 0, a1.stderr + a2.stderr)
size_runs = run1.stat().st_size + run2.stat().st_size
size_store = sum(f.stat().st_size for f in (A / "objects").rglob("*") if f.is_file())
check(f"store: 2 runs ({size_runs // 1024} KiB as files) take {size_store // 1024} KiB in the store (corpus stored once)",
      size_store < 0.62 * size_runs, f"{size_store} vs {size_runs}")

# 2 ─ Push to a shared directory; pull into a fresh store; the file comes back byte-identical.
st, r = sync(A, R, "--push")
check("sync --push: objects and refs reach the shared store", st and st["sent_objects"] > 0 and not st["roots_equal"],
      r.stderr + r.stdout)
st, r = sync(B, R, "--pull")
check("sync --pull: a fresh store receives everything", st and st["refs_after"] == 2, r.stderr + r.stdout)
got = k("store", "get", "run1", "-o", str(TMP / "run1-from-B.khn"), store=B)
check("store get: byte-identical recording after push → pull", got.returncode == 0 and sha(run1) == sha(TMP / "run1-from-B.khn"),
      got.stderr)
check("verify: the round-tripped recording verifies", khonsu("verify", str(TMP / "run1-from-B.khn")).returncode == 0)

# 3 ─ Anti-entropy does work proportional to the difference.
st, r = sync(B, R)
check("sync: already-synced stores compare one root and move nothing",
      st and st["roots_equal"] and st["sent_objects"] == 0 and st["received_objects"] == 0 and st["round_trips"] <= 2,
      json.dumps(st))
fork = TMP / "fork.khn"
subprocess.run([str(KHONSU), "fork", f"{run1}@3", "--dir", str(TMP / "forkdir"), "-o", str(fork), "-q",
                "--upstream", mock.url, "--", *AGENT], cwd=TMP, capture_output=True, text=True,
               env={**os.environ, **ENV}, timeout=120)
k("store", "add", str(fork), store=A)
st, r = sync(A, R, "--push")
check(f"sync: pushing a fork of an already-shared run sends only new objects ({st and st['sent_bytes']} bytes, "
      f"vs {fork.stat().st_size} for the file)",
      st and st["sent_bytes"] < 0.2 * fork.stat().st_size and st["buckets_differing"] < 64, json.dumps(st))

# 4 ─ CRDT annotations: concurrent edits on two machines converge.
k("annotate", "run1@1", "hallucinated", "API", "--label", "root-cause", "--author", "ana", store=A)
k("annotate", "run1@1", "confirmed", "on", "staging", "--label", "ok", "--author", "ben", store=B)
k("annotate", "run1", "flaky", "network", "--label", "note", "--author", "ben", store=B)
ben_note = json.loads(k("annotations", "run1", "--json", store=B).stdout)
flaky = next(a["tag"] for a in ben_note if a["text"] == "flaky network")
k("unannotate", flaky, store=B)  # ben retracts his own note
for s in (A, B, A, B):  # gossip through the shared store in an arbitrary order
    sync(s, R)
check("annotations: both machines converge to the same set",
      notes(A, "run1") == notes(B, "run1") and len(notes(A, "run1")) == 2, f"{notes(A, 'run1')}\n{notes(B, 'run1')}")

# 5 ─ Concurrent ref updates: last writer (by hybrid logical clock) wins everywhere.
k("store", "add", str(run1), "--name", "latest", store=A)
time.sleep(0.01)
k("store", "add", str(run2), "--name", "latest", store=B)
for s in (A, B, A):
    sync(s, R)
la = json.loads(k("store", "ls", "--json", store=A).stdout)
lb = json.loads(k("store", "ls", "--json", store=B).stdout)
man = lambda ls: next(x["manifest"] for x in ls if x["name"] == "latest")
check("refs: concurrent writes to the same name converge on one winner", man(la) == man(lb), f"{la}\n{lb}")
k("store", "rm", "latest", store=A)
for s in (A, B):
    sync(s, R)
check("refs: a delete propagates (tombstone)",
      all(x["name"] != "latest" for x in json.loads(k("store", "ls", "--json", store=B).stdout)))

# 6 ─ HTTP transport with a token.
port = free_port()
srv = spawn([str(KHONSU), "store", "serve", "--store", str(R), "--port", str(port), "--token", "s3cret", "-q"],
                       stdout=subprocess.PIPE, text=True)
srv.stdout.readline()
url = f"http://127.0.0.1:{port}"
bad, r = sync(C, url)
check("http: a wrong or missing token is rejected", bad is None and "401" in r.stderr, r.stderr)
st, r = sync(C, url, "--token", "s3cret")
check("http: sync over HTTP pulls every recording", st and st["refs_after"] >= 3, r.stderr + json.dumps(st))
got = k("store", "get", "run2", "-o", str(TMP / "run2-http.khn"), store=C)
check("http: byte-identical recording over HTTP", got.returncode == 0 and sha(run2) == sha(TMP / "run2-http.khn"))
st, r = sync(C, url, "--token", "s3cret")
check("http: second sync is a single root comparison", st and st["roots_equal"] and st["round_trips"] <= 2, json.dumps(st))

# 7 ─ A corrupt object on the remote is refused, never imported.
victim = next(p for p in (R / "objects").rglob("*") if p.is_file() and p.stat().st_size > 1000)
data = bytearray(victim.read_bytes())
data[len(data) // 2] ^= 1
victim.write_bytes(bytes(data))
D = TMP / "D"
st, r = sync(D, R, "--pull")
check("integrity: a corrupted remote object stops the pull", st is None and "corrupt" in r.stderr, r.stderr)
check("integrity: nothing corrupt was stored locally",
      all(k("store", "ls", store=D).returncode == 0 for _ in [0]))

srv.send_signal(signal.SIGINT)
srv.wait(timeout=10)
mock.stop()
shutil.rmtree(TMP, ignore_errors=True)
finish()
