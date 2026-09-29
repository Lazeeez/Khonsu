import { useEffect, useState } from "react";
import { Flaky } from "./components/Flaky";
import { MoonDial, type DialTick } from "./viewer/MoonDial";

const REPO_URL: string | undefined = import.meta.env.VITE_REPO_URL;

// A decorative run for the hero dial: model calls (gold), tools (faience), one failure.
const HERO_TICKS: DialTick[] = [
  "plan", "read_file", "draft", "write_file", "run_tests", "fix", "write_file", "run_tests", "review", "notify",
].map((label, i) => ({
  kind: ["read_file", "write_file", "run_tests", "notify"].includes(label) ? "tool" : "model",
  flagged: i === 4,
  label,
}));

const FEATURES = [
  { h: "Record any agent", p: "One environment variable. LangGraph, CrewAI, the OpenAI SDK, your own loop: if it calls an OpenAI-compatible API, Khonsu records every call, durably, even through kill -9.", code: "khonsu record -- python agent.py" },
  { h: "Replay, bit-exact", p: "Same run, zero tokens, zero network. The model can be offline. If anything drifts, Khonsu names the exact JSON field or file that changed.", code: "khonsu replay run.khn -- python agent.py" },
  { h: "Travel the workspace", p: "The working directory is snapshotted before every call as a Merkle tree inside the recording. See any file at any moment.", code: "khonsu cat run.khn@4 src/app.py" },
  { h: "Fork from any call", p: "Restore the files, replay calls before N, go live from N. Pay only for what changed; side effects before the fork don't repeat.", code: "khonsu fork run.khn@4 -- python agent.py" },
  { h: "Tests that never flake", p: "Mark a pytest with a recording: offline, deterministic, free. A prompt change fails with the field that changed.", code: "@pytest.mark.khonsu(\"refund.khn\")" },
  { h: "Share without conflicts", p: "Merkle anti-entropy sync over a folder or HTTP moves only what differs. Notes merge as CRDTs: no coordinator, nothing lost.", code: "khonsu sync http://khonsu.internal" },
];

const STATS = [
  { v: "76%", k: "of tokens reused when you edit one prompt" },
  { v: "80 / 80", k: "kill -9 trials: every observed response survived" },
  { v: "34 ms", k: "snapshot of a 20,000-file repo" },
  { v: "4 KiB", k: "sent to share a 1.37 MB fork" },
];

export function Landing() {
  const [dial, setDial] = useState(0);
  const [auto, setAuto] = useState(true);
  useEffect(() => {
    if (!auto || window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    const id = setInterval(() => setDial((d) => (d + 1) % HERO_TICKS.length), 900);
    return () => clearInterval(id);
  }, [auto]);

  return (
    <main className="landing">
      <section className="hero">
        <div className="hero-copy">
          <p className="eyebrow">Khonsu · the traveller</p>
          <h1 className="display">Travel back to the moment your agent went wrong.</h1>
          <p className="lede">
            Khonsu records every model call, tool call and file change an AI agent makes, then lets you replay the run
            bit-exactly, inspect any moment, and fork a new future from it. It's free, local, and works with any framework.
          </p>
          <div className="cta">
            <a className="btn primary big" href="#/viewer?src=coding-agent.khn">Open a recorded run</a>
            <a className="btn big" href="#/viewer">Drop your own .khn</a>
            {REPO_URL && <a className="btn ghost big" href={REPO_URL} target="_blank" rel="noreferrer">Install the CLI</a>}
          </div>
        </div>
        <div className="hero-dial" onPointerDown={() => setAuto(false)}>
          <MoonDial ticks={HERO_TICKS} value={dial} onChange={(i) => { setAuto(false); setDial(i); }} size={340}
            caption={`${HERO_TICKS[dial].kind === "tool" ? "tool" : "model"} · ${HERO_TICKS[dial].label}${HERO_TICKS[dial].flagged ? "  ✕ tests failed here" : ""}`} />
        </div>
      </section>

      <section className="terminal panel" aria-label="Example session">
        <pre>
<span className="p">$</span> khonsu record -- python agent.py{"\n"}
<span className="d">☾ khonsu recorded 7 calls (5 tools) · 9 workspace snapshots → run.khn (4.5 KiB, fsync'd)</span>{"\n"}
<span className="p">$</span> khonsu replay run.khn -- python agent.py      <span className="d"># model offline? doesn't matter</span>{"\n"}
<span className="d">☾ khonsu replayed 7/7 from recording · 0 live · 0 divergences · workspace identical</span>{"\n"}
<span className="p">$</span> khonsu diff yesterday.khn today.khn{"\n"}
<span className="g">  first divergence</span> A#0 / B#0: same request, different output{"\n"}
    A  → get_weather({"{"}"city": "Luxor"{"}"}){"\n"}
    B  → get_weather({"{"}"city": "Giza"{"}"}){"\n"}
  <span className="g">≠</span> A#1   B#1   at messages[2].tool_calls[0].function.arguments
        </pre>
      </section>

      <Flaky />

      <section className="features">
        {FEATURES.map((f) => (
          <article key={f.h} className="feature">
            <h3>{f.h}</h3>
            <p>{f.p}</p>
            <code>{f.code}</code>
          </article>
        ))}
      </section>

      <section className="stats">
        {STATS.map((s) => (
          <div key={s.k} className="stat"><div className="v">{s.v}</div><div className="k">{s.k}</div></div>
        ))}
      </section>

      <section className="also">
        <a className="also-card" href="#/play">
          <h3>Play: fork a running program</h3>
          <p>A tiny deterministic VM keeps its whole state in Khonsu's Merkle store. Fork it mid-loop in microseconds, change a variable in the fork, or run a dozen futures at once and watch which pages each one copies.</p>
        </a>
        <a className="also-card" href="#/simulator">
          <h3>Simulator</h3>
          <p>The deterministic kernel compiled to WebAssembly: fork a multi-agent run in ~1 µs, speculate 8 futures, tamper with history and watch verification catch it.</p>
        </a>
        <a className="also-card" href="#/bench">
          <h3>Benchmarks</h3>
          <p>Khonsu against vcrpy and LangGraph checkpointers, and the engine's own suite run on your device: persistent Merkle state versus page tables versus deep copies.</p>
        </a>
      </section>

      <footer className="foot">
        <span>Khonsu (<span className="glyphs" lang="egy">𓐍𓈖𓋴𓅱</span>) is the Egyptian god of the moon, whose name means <i>traveller</i>.</span>
        <span className="muted">Everything on this site runs in your browser.</span>
      </footer>
    </main>
  );
}
