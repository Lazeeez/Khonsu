import { useEffect, useRef, useState } from "react";
import { Flaky } from "./components/Flaky";
import { HeroDial } from "./components/HeroDial";
import { reducedMotion, spotlight, useCountUp, useReveal } from "./lib/motion";
import type { DialTick } from "./viewer/MoonDial";

// A run for the hero dial: model calls (gold), tools (faience), one failing test run.
const HERO_TICKS: DialTick[] = [
  "plan", "read_file", "draft", "write_file", "run_tests", "fix", "write_file", "run_tests", "review", "notify",
].map((label, i) => ({
  kind: ["read_file", "write_file", "run_tests", "notify"].includes(label) ? "tool" : "model",
  flagged: i === 4,
  label,
}));

const FEATURES = [
  { g: "◉", h: "Record any agent", p: "One environment variable. LangGraph, CrewAI, the OpenAI SDK, your own loop: if it calls an OpenAI-compatible API, Khonsu records every call, durably, even through kill -9.", code: "khonsu record -- python agent.py" },
  { g: "↺", h: "Replay, bit-exact", p: "Same run, zero tokens, zero network. The model can be offline. If anything drifts, Khonsu names the exact JSON field or file that changed.", code: "khonsu replay run.khn -- python agent.py" },
  { g: "▤", h: "Travel the workspace", p: "The working directory is snapshotted before every call as a Merkle tree inside the recording. See any file at any moment.", code: "khonsu cat run.khn@4 src/app.py" },
  { g: "⑂", h: "Fork from any call", p: "Restore the files, replay calls before N, go live from N. Pay only for what changed; side effects before the fork don't repeat.", code: "khonsu fork run.khn@4 -- python agent.py" },
  { g: "✓", h: "Tests that never flake", p: "Mark a pytest with a recording: offline, deterministic, free. A prompt change fails with the field that changed.", code: "@pytest.mark.khonsu(\"refund.khn\")" },
  { g: "⇄", h: "Share without conflicts", p: "Merkle anti-entropy sync over a folder or HTTP moves only what differs. Notes merge as CRDTs: no coordinator, nothing lost.", code: "khonsu sync http://khonsu.internal" },
];

const STATS: { to: number; fmt: (v: number) => string; k: string }[] = [
  { to: 76, fmt: (v) => `${Math.round(v)}%`, k: "of tokens reused when you edit one prompt" },
  { to: 24, fmt: (v) => `${Math.round(v)} / 24`, k: "offline replays identical to the recorded run" },
  { to: 80, fmt: (v) => `${Math.round(v)} / 80`, k: "kill -9 trials: every observed response survived" },
  { to: 34, fmt: (v) => `${Math.round(v)} ms`, k: "to snapshot a 20,000-file repository" },
];

const TERMINAL: [string, string][] = [
  ["p", "khonsu record -- python agent.py"],
  ["d", "☾ khonsu recorded 7 calls (5 tools) · 9 workspace snapshots → run.khn (4.5 KiB, fsync'd)"],
  ["p", "khonsu replay run.khn -- python agent.py      # model offline? doesn't matter"],
  ["d", "☾ khonsu replayed 7/7 from recording · 0 live · 0 divergences · workspace identical"],
  ["p", "khonsu diff yesterday.khn today.khn"],
  ["g", "  first divergence  A#0 / B#0: same request, different output"],
  ["", "    A  → get_weather({\"city\": \"Luxor\"})"],
  ["", "    B  → get_weather({\"city\": \"Giza\"})"],
  ["g", "  ≠ A#1   B#1   at messages[2].tool_calls[0].function.arguments"],
];

const EXPLORE = [
  { href: "#/play", h: "Play: fork a running program", p: "A tiny deterministic VM keeps its whole state in Khonsu's Merkle store. Fork it mid-loop in microseconds, change a variable in the fork, or run a dozen futures at once and watch which pages each one copies." },
  { href: "#/simulator", h: "Simulator", p: "The deterministic kernel compiled to WebAssembly: fork a multi-agent run in about a microsecond, speculate eight futures, tamper with history and watch verification catch it." },
  { href: "#/bench", h: "Benchmarks", p: "Khonsu against vcrpy and LangGraph checkpointers, and the engine's own suite run on your device: persistent Merkle state versus page tables versus deep copies." },
];

const delay = (ms: number) => ({ ["--d" as string]: `${ms}ms` });
const index = (i: number) => ({ ["--i" as string]: i });

function Stat({ to, fmt, k, on }: { to: number; fmt: (v: number) => string; k: string; on: boolean }) {
  const v = useCountUp(to, on);
  return (
    <div className="stat">
      <div className="v">{fmt(v)}</div>
      <div className="k">{k}</div>
    </div>
  );
}

function Stats() {
  const ref = useRef<HTMLElement>(null);
  const [on, setOn] = useState(false);
  useEffect(() => {
    const el = ref.current;
    if (!el || typeof IntersectionObserver === "undefined") {
      setOn(true);
      return;
    }
    const io = new IntersectionObserver(([e]) => {
      if (e.isIntersecting) {
        setOn(true);
        io.disconnect();
      }
    }, { threshold: 0.3 });
    io.observe(el);
    return () => io.disconnect();
  }, []);
  return (
    <section className="stats reveal" ref={ref} aria-label="Measured results">
      {STATS.map((s) => <Stat key={s.k} {...s} on={on} />)}
    </section>
  );
}

/** Scroll drift for the hero and a gentle 3D tilt of the moon toward the pointer. */
function useHeroMotion(hero: React.RefObject<HTMLElement | null>, dial: React.RefObject<HTMLDivElement | null>) {
  useEffect(() => {
    const h = hero.current, d = dial.current;
    if (!h || !d || reducedMotion()) return;
    let raf = 0;
    const onScroll = () => {
      if (raf) return;
      raf = requestAnimationFrame(() => {
        raf = 0;
        const p = Math.min(1, scrollY / Math.max(1, h.offsetHeight));
        h.style.setProperty("--scroll", p.toFixed(3));
      });
    };
    const fine = matchMedia("(pointer: fine)").matches;
    const onMove = (e: PointerEvent) => {
      const r = d.getBoundingClientRect();
      const x = (e.clientX - (r.left + r.width / 2)) / r.width;
      const y = (e.clientY - (r.top + r.height / 2)) / r.height;
      d.style.setProperty("--ry", `${Math.max(-1, Math.min(1, x)) * 7}deg`);
      d.style.setProperty("--rx", `${Math.max(-1, Math.min(1, -y)) * 7}deg`);
    };
    const onLeave = () => {
      d.style.setProperty("--ry", "0deg");
      d.style.setProperty("--rx", "0deg");
    };
    addEventListener("scroll", onScroll, { passive: true });
    if (fine) {
      h.addEventListener("pointermove", onMove);
      h.addEventListener("pointerleave", onLeave);
    }
    return () => {
      cancelAnimationFrame(raf);
      removeEventListener("scroll", onScroll);
      h.removeEventListener("pointermove", onMove);
      h.removeEventListener("pointerleave", onLeave);
    };
  }, [hero, dial]);
}

export function Landing() {
  const root = useRef<HTMLElement>(null);
  const hero = useRef<HTMLElement>(null);
  const dial = useRef<HTMLDivElement>(null);
  useReveal(root);
  useHeroMotion(hero, dial);
  return (
    <main className="landing" ref={root}>
      <section className="hero" ref={hero}>
        <div className="hero-copy">
          <p className="eyebrow enter" style={delay(0)}>Khonsu · the traveller</p>
          <h1 className="display enter" style={delay(90)}>Travel back to the moment your agent went wrong.</h1>
          <p className="lede enter" style={delay(180)}>
            Khonsu records every model call, tool call and file change an AI agent makes, then lets you replay the run
            bit-exactly, inspect any moment, and fork a new future from it. Free, local, and it works with any framework.
          </p>
          <div className="cta enter" style={delay(270)}>
            <a className="btn primary big" href="#/viewer?src=coding-agent.khn">Open a recorded run <span aria-hidden>→</span></a>
            <a className="btn big glass" href="#/downloads">Download Khonsu</a>
          </div>
          <p className="hero-note enter" style={delay(360)}>
            New here? <a href="#/guide">Read the two-minute guide</a> first. Everything on this site runs in your browser.
          </p>
        </div>
        <div className="hero-dial moonrise" ref={dial}>
          <div className="rays" aria-hidden />
          <div className="tilt">
            <HeroDial ticks={HERO_TICKS} />
          </div>
        </div>
      </section>

      <section className="terminal reveal" aria-label="Example session">
        <div className="term-bar" aria-hidden><i /><i /><i /><span>~/agent</span></div>
        <pre>
          {TERMINAL.map(([cls, text], i) => (
            <span key={i} className={`tl ${cls}`} style={index(i)}>
              {cls === "p" && <span className="prompt">$ </span>}
              {text}
              {"\n"}
            </span>
          ))}
        </pre>
      </section>

      <div className="reveal"><Flaky /></div>

      <section className="features" aria-label="What Khonsu does">
        {FEATURES.map((f, i) => (
          <article key={f.h} className="feature spot reveal" style={index(i % 3)} onPointerMove={spotlight}>
            <div className="feature-g" aria-hidden>{f.g}</div>
            <h3>{f.h}</h3>
            <p>{f.p}</p>
            <code>{f.code}</code>
          </article>
        ))}
      </section>

      <Stats />

      <section className="also" aria-label="Explore">
        {EXPLORE.map((c, i) => (
          <a key={c.href} className="also-card spot reveal" href={c.href} style={index(i)} onPointerMove={spotlight}>
            <h3>{c.h}</h3>
            <p>{c.p}</p>
            <span className="also-go" aria-hidden>→</span>
          </a>
        ))}
      </section>

      <footer className="foot">
        <span>Khonsu (<span className="glyphs" lang="egy">𓐍𓈖𓋴𓅱</span>) is the Egyptian god of the moon, whose name means <i>traveller</i>.</span>
        <span className="muted">Nothing you open here leaves your machine.</span>
      </footer>
    </main>
  );
}
