"""Khonsu vs vcrpy (the standard Python HTTP-cassette library) on the same agent traffic.

Measures, with the official OpenAI SDK against the local test model:
  1. per-call latency: direct, through Khonsu (record / replay), with vcrpy (record / playback);
  2. durability: SIGKILL mid-run, then count how many completed calls each tool kept;
  3. secrets: whether the API key ends up in the recording with default settings;
  4. a changed prompt: does replay reuse the unchanged calls and pinpoint the change?
"""

import os
import signal
import subprocess
import tempfile
import textwrap
import time
from pathlib import Path

from common import KHONSU, PY, Proc, khonsu_serve, mock_server, now, summarize_ms, write_result

N = int(os.environ.get("BENCH_CALLS", "300"))
TMP = Path(tempfile.mkdtemp(prefix="khonsu-vcr-"))
KEY = "sk-bench-SECRET-0123456789"

# One agent script, parameterized by mode, so every tool sees identical traffic.
AGENT = TMP / "agent.py"
AGENT.write_text(textwrap.dedent(f'''
    import os, sys, time, json
    from openai import OpenAI
    mode, n = sys.argv[1], int(sys.argv[2])
    cassette = os.environ.get("CASSETTE")
    def run(client):
        lat = []
        for i in range(n):
            t = time.perf_counter()
            r = client.chat.completions.create(model="mock-1",
                messages=[{{"role": "user", "content": f"step {{i}}: " + os.environ.get("PROMPT_SUFFIX", "")}}])
            lat.append(time.perf_counter() - t)
            print("done", i, flush=True)
        print("LAT " + json.dumps(lat), flush=True)
    client = OpenAI(api_key="{KEY}", max_retries=0)
    if mode == "vcr":
        import vcr
        rec_mode = os.environ.get("VCR_MODE", "once")
        with vcr.use_cassette(cassette, record_mode=rec_mode, match_on=["method", "uri", "body"]):
            run(client)
    else:
        run(client)
'''))


def lat_of(out):
    import json
    line = next(l for l in out.splitlines() if l.startswith("LAT "))
    return json.loads(line[4:])


def run_agent(mode, n, env):
    r = subprocess.run([PY, str(AGENT), mode, str(n)], capture_output=True, text=True, timeout=600,
                       env={**os.environ, **env})
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-2000:])
    return r


results = {"calls": N}
mock, url = mock_server("0")
try:
    # ── 1. latency ─────────────────────────────────────────────────────────────────────────
    direct = lat_of(run_agent("plain", N, {"OPENAI_BASE_URL": url}).stdout)
    rec_file = TMP / "run.khn"
    kp, base = khonsu_serve("--record", str(rec_file), "--upstream", url)
    k_record = lat_of(run_agent("plain", N, {"OPENAI_BASE_URL": base}).stdout)
    kp.stop()
    kp, base = khonsu_serve("--replay", str(rec_file))
    k_replay = lat_of(run_agent("plain", N, {"OPENAI_BASE_URL": base}).stdout)
    kp.stop()
    cas = TMP / "run.yaml"
    v_record = lat_of(run_agent("vcr", N, {"OPENAI_BASE_URL": url, "CASSETTE": str(cas)}).stdout)
    v_play = lat_of(run_agent("vcr", N, {"OPENAI_BASE_URL": url, "CASSETTE": str(cas), "VCR_MODE": "none"}).stdout)
    results["latency"] = {
        "direct": summarize_ms(direct),
        "khonsu_record": summarize_ms(k_record),
        "khonsu_replay": summarize_ms(k_replay),
        "vcrpy_record": summarize_ms(v_record),
        "vcrpy_playback": summarize_ms(v_play),
    }
    results["storage_bytes"] = {"khonsu": rec_file.stat().st_size, "vcrpy": cas.stat().st_size}

    # ── 2. secrets with default settings ───────────────────────────────────────────────────────
    results["api_key_in_recording"] = {"khonsu": KEY.encode() in rec_file.read_bytes(),
                                       "vcrpy": KEY.encode() in cas.read_bytes()}

    # ── 3. durability: SIGKILL after ~half the calls ───────────────────────────────────────────
    def killed(mode):
        m, u = mock_server("0.01")
        try:
            out_file = TMP / f"killed-{mode}"
            out_file.unlink(missing_ok=True)
            if mode == "khonsu":
                args = [KHONSU, "record", "-q", "--no-workspace", "-o", str(out_file), "--upstream", u, "--", PY, str(AGENT), "plain", "60"]
                env = {}
            else:
                args = [PY, str(AGENT), "vcr", "60"]
                env = {"OPENAI_BASE_URL": u, "CASSETTE": str(out_file)}
            p = subprocess.Popen(args, stdout=subprocess.PIPE, text=True, env={**os.environ, **env}, start_new_session=True)
            seen = 0
            for line in p.stdout:
                if line.startswith("done"):
                    seen += 1
                    if seen == 30:
                        os.killpg(p.pid, signal.SIGKILL)  # the whole process group, like an OOM kill or a crashed CI runner
                        break
            p.wait()
            kept = 0
            if out_file.exists():
                if mode == "khonsu":
                    import json as _j
                    r = subprocess.run([KHONSU, "show", str(out_file), "--json"], capture_output=True, text=True)
                    kept = sum(1 for e in _j.loads(r.stdout)["exchanges"] if "status" in e) if r.returncode == 0 else 0
                else:
                    kept = out_file.read_text().count("- request:")
            return {"calls_completed_before_kill": seen, "calls_in_recording": kept}
        finally:
            m.stop()

    results["kill_minus_9"] = {"khonsu": killed("khonsu"), "vcrpy": killed("vcr")}

    # ── 4. a changed prompt ───────────────────────────────────────────────────────────────────
    # Record 20 calls, then change the prompt suffix only from call 15 on (as a later-step
    # template change would): how many calls does each tool serve without the model?
    small = TMP / "twenty.khn"
    subprocess.run([KHONSU, "record", "-q", "--no-workspace", "-o", str(small), "--upstream", url, "--",
                    PY, str(AGENT), "plain", "20"], check=True, capture_output=True)
    # the agent script builds prompts from PROMPT_SUFFIX for all calls; emulate a late change by
    # recording with an agent variant that appends the suffix only for i >= 15
    AGENT2 = TMP / "agent_late.py"
    AGENT2.write_text(AGENT.read_text().replace(
        'f"step {i}: " + os.environ.get("PROMPT_SUFFIX", "")',
        'f"step {i}: " + (os.environ.get("PROMPT_SUFFIX", "") if i >= 15 else "")'))
    r = subprocess.run([KHONSU, "replay", str(small), "--on-miss", "live", "--upstream", url, "--",
                        PY, str(AGENT2), "plain", "20"], capture_output=True, text=True, env={**os.environ, "PROMPT_SUFFIX": " (v2)"})
    replayed = int(r.stderr.split("replayed ")[1].split("/")[0]) if "replayed " in r.stderr else -1
    field = "messages[0].content" if "messages[0].content" in r.stderr else ""
    cas20 = TMP / "twenty.yaml"
    run_agent("vcr", 20, {"OPENAI_BASE_URL": url, "CASSETTE": str(cas20)})
    v = subprocess.run([PY, str(AGENT2), "vcr", "20"], capture_output=True, text=True,
                       env={**os.environ, "OPENAI_BASE_URL": url, "CASSETTE": str(cas20), "VCR_MODE": "none", "PROMPT_SUFFIX": " (v2)"})
    results["prompt_change_at_call_15_of_20"] = {
        "khonsu": {"served_from_recording": replayed, "went_live": 20 - replayed, "divergence_field_reported": field},
        "vcrpy_record_mode_none": {"agent_succeeded": v.returncode == 0,
                                   "error": (v.stderr.strip().splitlines() or [""])[-1][:160]},
    }
finally:
    mock.stop()

write_result("vcr", results)
