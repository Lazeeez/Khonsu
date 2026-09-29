"""Khonsu vs LangGraph checkpointers.

LangGraph "time travel" resumes a graph from a stored checkpoint. That restores
the graph *state*, then calls the model again, so with a sampling model the
continuation differs, and tools re-run with their side effects. Khonsu replays
the recorded responses instead.

Measures, on the same tool-using agent (LangGraph prebuilt ReAct agent,
ChatOpenAI pointed at the non-deterministic test model):
  1. reproducibility: resume from a mid-run checkpoint K times (LangGraph) vs
     replay the Khonsu recording K times; count distinct final outcomes;
  2. side effects on time travel: how many times the tool re-executes;
  3. storage growth vs run length (SqliteSaver database vs .khn recording);
  4. per-run overhead: no persistence, InMemorySaver, SqliteSaver, Khonsu.
"""

import os
import sqlite3
import subprocess
import tempfile
import textwrap
from pathlib import Path

from common import KHONSU, PY, khonsu_serve, mock_server, now, write_result

TMP = Path(tempfile.mkdtemp(prefix="khonsu-lg-"))
K = int(os.environ.get("BENCH_TRIALS", "10"))

AGENT = TMP / "lg_agent.py"
AGENT.write_text(textwrap.dedent('''
    import json, os, sys, sqlite3, time
    from langchain_core.tools import tool
    from langchain_openai import ChatOpenAI
    from langgraph.prebuilt import create_react_agent
    from langgraph.checkpoint.memory import InMemorySaver
    from langgraph.checkpoint.sqlite import SqliteSaver

    SIDE = os.environ["SIDE_EFFECTS"]

    @tool
    def get_weather(city: str) -> str:
        """Weather for a city."""
        with open(SIDE, "a") as f:
            f.write(city + "\\n")          # an observable side effect, like sending a request
        return f"{city}: {20 + len(city)}C"

    mode, turns = sys.argv[1], int(sys.argv[2])
    llm = ChatOpenAI(model="mock-1", temperature=0.8, max_retries=0)
    saver = None
    if mode == "memory":
        saver = InMemorySaver()
    elif mode.startswith("sqlite"):
        saver = SqliteSaver(sqlite3.connect(os.environ["DB"], check_same_thread=False))
    agent = create_react_agent(llm, [get_weather], checkpointer=saver)
    cfg = {"configurable": {"thread_id": "t1"}}

    if mode == "sqlite-resume":
        # LangGraph time travel: pick the checkpoint just before the final answer and resume from it.
        history = list(agent.get_state_history(cfg))
        target = next(s for s in history if s.next == ("agent",) and len(s.values["messages"]) >= 3)
        out = agent.invoke(None, target.config)
        print("FINAL " + out["messages"][-1].content)
        sys.exit(0)

    t = time.perf_counter()
    for i in range(turns):
        out = agent.invoke({"messages": [("user", f"turn {i}: what is the weather where the moon god lives?")]},
                           cfg if saver else None)
    print("ELAPSED " + str(time.perf_counter() - t))
    print("FINAL " + out["messages"][-1].content)
'''))


def run(mode, turns, env):
    r = subprocess.run([PY, str(AGENT), mode, str(turns)], capture_output=True, text=True, timeout=900,
                       env={**os.environ, **env})
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-3000:])
    return r.stdout


def final(out):
    return next(l for l in out.splitlines() if l.startswith("FINAL ")).removeprefix("FINAL ")


def elapsed(out):
    return float(next(l for l in out.splitlines() if l.startswith("ELAPSED ")).split()[1])


results = {}
mock, url = mock_server("0")
side = TMP / "side.log"
base_env = {"OPENAI_BASE_URL": url, "OPENAI_API_KEY": "x", "SIDE_EFFECTS": str(side)}
try:
    # ── 1+2. time travel: LangGraph resume vs Khonsu replay ───────────────────────────────────
    db = TMP / "tt.sqlite"
    run("sqlite", 1, {**base_env, "DB": str(db)})
    side.write_text("")
    lg_finals = [final(run("sqlite-resume", 1, {**base_env, "DB": str(db)})) for _ in range(K)]
    lg_side = len(side.read_text().splitlines())

    rec = TMP / "tt.khn"
    subprocess.run([KHONSU, "record", "-q", "--no-workspace", "-o", str(rec), "--upstream", url, "--",
                    PY, str(AGENT), "none", "1"], check=True, capture_output=True,
                   env={**os.environ, "SIDE_EFFECTS": str(side)})
    kh_finals = []
    for _ in range(K):
        out = subprocess.run([KHONSU, "replay", str(rec), "-q", "--", PY, str(AGENT), "none", "1"], check=True,
                             capture_output=True, text=True, env={**os.environ, "SIDE_EFFECTS": str(side)}).stdout
        kh_finals.append(final(out))
    results["time_travel"] = {
        "trials": K,
        "langgraph_resume_distinct_outcomes": len(set(lg_finals)),
        "khonsu_replay_distinct_outcomes": len(set(kh_finals)),
        "langgraph_tool_reexecutions_during_resume": lg_side,
        "note": "LangGraph restores state then re-calls the model; Khonsu serves recorded responses. "
                "Khonsu's undecorated tool still re-runs; with @khonsu.tool it would not.",
    }

    # ── 3. storage growth vs run length ────────────────────────────────────────────────────────
    growth = []
    for turns in (1, 5, 10, 20, 40):
        db = TMP / f"g{turns}.sqlite"
        run("sqlite", turns, {**base_env, "DB": str(db)})
        con = sqlite3.connect(db)
        payload = con.execute("select coalesce(sum(length(checkpoint)),0) + coalesce(sum(length(metadata)),0) from checkpoints").fetchone()[0]
        writes = con.execute("select coalesce(sum(length(value)),0) from writes").fetchone()[0]
        checkpoints = con.execute("select count(*) from checkpoints").fetchone()[0]
        con.close()
        rec = TMP / f"g{turns}.khn"
        subprocess.run([KHONSU, "record", "-q", "--no-workspace", "-o", str(rec), "--upstream", url, "--",
                        PY, str(AGENT), "memory", str(turns)], check=True, capture_output=True,
                       env={**os.environ, "SIDE_EFFECTS": str(side)})
        growth.append({"turns": turns, "langgraph_checkpoints": checkpoints,
                       "langgraph_sqlite_payload_bytes": payload + writes,
                       "khonsu_recording_bytes": rec.stat().st_size})
    results["storage_growth"] = growth

    # ── 4. overhead per 10-turn run ────────────────────────────────────────────────────────────
    def best_of(n, f):
        return min(f() for _ in range(n))
    over = {}
    # Same accumulating 10-turn conversation everywhere; InMemorySaver is the baseline.
    over["inmemory_saver_s"] = best_of(3, lambda: elapsed(run("memory", 10, base_env)))
    def sq():
        db = TMP / f"o{now()}.sqlite"
        return elapsed(run("sqlite", 10, {**base_env, "DB": str(db)}))
    over["sqlite_saver_s"] = best_of(3, sq)
    def kh():
        p, b = khonsu_serve("--record", str(TMP / f"o{now()}.khn"), "--upstream", url)
        try:
            return elapsed(run("memory", 10, {**base_env, "OPENAI_BASE_URL": b}))
        finally:
            p.stop()
    over["inmemory_saver_plus_khonsu_record_s"] = best_of(3, kh)
    results["overhead_10_turns"] = {k: round(v, 4) for k, v in over.items()}
finally:
    mock.stop()

write_result("langgraph", results)
