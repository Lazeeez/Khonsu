import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Bench } from "./components/Bench";
import { Inspector, ownerOf, type Tab } from "./components/Inspector";
import { Multiverse, type Sel } from "./components/Multiverse";
import { PageMap } from "./components/PageMap";
import { Trace } from "./components/Trace";
import { KernelClient } from "./kernel/client";
import type { StateInfo, VerifyResult, World } from "./kernel/types";
import { ACTORS } from "./kernel/types";
import { ACTOR_COLORS, download, fmtBytes, fmtInt, laneColor } from "./lib/ui";

const REPO_URL: string | undefined = import.meta.env.VITE_REPO_URL;
const HINT = "Use api.fetchUser(id) from src/api.ts. There is no getUserById.";

type Toast = { id: number; msg: string; kind?: "good" | "bad" };

export default function App() {
  const [client] = useState(() => new KernelClient());
  const [route, setRoute] = useState(() => location.hash);
  useEffect(() => {
    const on = () => setRoute(location.hash);
    addEventListener("hashchange", on);
    return () => removeEventListener("hashchange", on);
  }, []);
  const isBench = route.startsWith("#/bench");

  return (
    <div className="app">
      <TopBar isBench={isBench} client={client} />
      {isBench ? <Bench client={client} /> : <Workbench client={client} />}
    </div>
  );
}

// ---------------------------------------------------------------------------------------------

function Moon({ size = 26 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 64 64" aria-hidden>
      <defs>
        <linearGradient id="mg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="#fff3d6" />
          <stop offset="1" stopColor="#f5c26b" />
        </linearGradient>
      </defs>
      <path d="M40 8a24 24 0 1 0 16 40A19 19 0 1 1 40 8z" fill="url(#mg)" />
    </svg>
  );
}

const hudListeners = new Set<(w: World) => void>();
let hudForkCost = 0;

function TopBar({ isBench }: { isBench: boolean; client: KernelClient }) {
  const [w, setW] = useState<World | null>(null);
  useEffect(() => {
    hudListeners.add(setW);
    return () => void hudListeners.delete(setW);
  }, []);
  const totals = useMemo(() => {
    if (!w) return null;
    let calls = 0, hits = 0, saved = 0;
    for (const t of w.timelines) {
      calls += t.llm_calls;
      hits += t.cache_hits;
      saved += t.tokens_saved;
    }
    return { calls, hits, saved };
  }, [w]);
  return (
    <header className="topbar">
      <div className="brand">
        <Moon />
        <div style={{ minWidth: 0 }}>
          <h1>KHONSU</h1>
          <div className="sub">deterministic, forkable runtime for multi-agent LLM systems · C++23 → WebAssembly</div>
        </div>
      </div>
      <nav className="nav">
        <a href="#/" className={!isBench ? "active" : ""}>Workbench</a>
        <a href="#/bench" className={isBench ? "active" : ""}>Benchmarks</a>
        {REPO_URL && <a href={`${REPO_URL}/blob/main/docs/DESIGN.md`} target="_blank" rel="noreferrer">Design</a>}
        {REPO_URL && <a href={REPO_URL} target="_blank" rel="noreferrer">GitHub</a>}
      </nav>
      <div className="spacer" />
      {w && totals && (
        <div className="hud" aria-label="Kernel metrics">
          <Metric k="fork() cost" v={hudForkCost ? `${(hudForkCost * 1000).toFixed(0)} ns` : "n/a"} hot />
          <Metric k="avg step" v={`${w.metrics.avg_step_us.toFixed(0)} µs`} />
          <Metric k="timelines" v={fmtInt(w.timelines.length)} />
          <Metric k="oracle cache" v={`${totals.hits}/${totals.calls}`} />
          <Metric k="tokens saved" v={fmtInt(totals.saved)} />
          <Metric k="live state" v={fmtBytes(w.store.bytes)} />
          <Metric k="wasm heap" v={fmtBytes(w.heap_bytes)} />
        </div>
      )}
    </header>
  );
}

function Metric({ k, v, hot }: { k: string; v: string; hot?: boolean }) {
  return (
    <div className={`metric ${hot ? "hot" : ""}`}>
      <span className="v">{v}</span>
      <span className="k">{k}</span>
    </div>
  );
}

// ---------------------------------------------------------------------------------------------

function Workbench({ client }: { client: KernelClient }) {
  const [world, setWorld] = useState<World | null>(null);
  const [sel, setSel] = useState<Sel>({ tid: 0, g: 0 });
  const [state, setState] = useState<StateInfo | null>(null);
  const [tab, setTab] = useState<Tab>("step");
  const [version, setVersion] = useState(0);
  const [verify, setVerify] = useState<VerifyResult | null>(null);
  const [playing, setPlaying] = useState(false);
  const [toasts, setToasts] = useState<Toast[]>([]);
  const [consoleLines, setConsole] = useState<{ t: string; m: string }[]>([]);
  const [injectOpen, setInjectOpen] = useState(false);
  const [chapters, setChapters] = useState<Set<number>>(new Set());
  const [busy, setBusy] = useState(false);
  const [fatal, setFatal] = useState<string | null>(null);
  const forkCost = useRef(0);  // amortized µs per Kernel::fork (single samples hit timer coarsening)
  const worldRef = useRef<World | null>(null);
  worldRef.current = world;

  const toast = useCallback((msg: string, kind?: Toast["kind"]) => {
    const id = Math.random();
    setToasts((t) => [...t.slice(-3), { id, msg, kind }]);
    setTimeout(() => setToasts((t) => t.filter((x) => x.id !== id)), 5200);
    const now = new Date();
    setConsole((c) => [...c.slice(-80), { t: now.toTimeString().slice(0, 8), m: msg }]);
  }, []);

  const refresh = useCallback(async () => {
    const w = await client.world();
    setWorld(w);
    hudListeners.forEach((f) => f(w));
    setVersion((v) => v + 1);
    return w;
  }, [client]);

  // Boot: load the scenario and record the "incident" run.
  useEffect(() => {
    (async () => {
      try {
        const t0 = performance.now();
        forkCost.current = await client.forkCost();
        hudForkCost = forkCost.current;
        await client.init();
        await client.step(0, 200);
        const w = await refresh();
        setSel({ tid: 0, g: w.timelines[0].head });
        toast(`Kernel ready in ${(performance.now() - t0).toFixed(0)} ms. Recorded run of issue #42: ${w.timelines[0].status}.`);
      } catch (e) {
        setFatal(String(e));
      }
    })();
  }, [client, refresh, toast]);

  useEffect(() => {
    if (!world) return;
    let live = true;
    client.state(sel.tid, sel.g).then((s) => live && setState(s)).catch(() => live && setState(null));
    return () => {
      live = false;
    };
  }, [client, sel, version, world]);

  const run = useCallback(
    async (label: string, f: () => Promise<void>) => {
      if (busy) return;
      setBusy(true);
      try {
        await f();
      } catch (e) {
        toast(`${label} failed: ${e instanceof Error ? e.message : e}`, "bad");
      } finally {
        setBusy(false);
      }
    },
    [busy, toast],
  );

  const tl = world?.timelines[sel.tid];

  // ---- actions ------------------------------------------------------------------------------
  const stepLive = useCallback(async (tid: number, n: number) => {
    const r = await client.step(tid, n);
    const w = await refresh();
    setSel({ tid, g: w.timelines[tid].head });
    return r.executed;
  }, [client, refresh]);

  const doFork = useCallback(async (g = sel.g, tid = sel.tid) => {
    const r = await client.fork(tid, g);
    const w = await refresh();
    const t = w.timelines[r.id];
    setSel({ tid: r.id, g });
    toast(`Forked ${t.name} from ${w.timelines[tid].name} @ step ${g}: one root handle copied, 0 pages (≈${forkCost.current ? (forkCost.current * 1000).toFixed(0) : "<1000"} ns amortized).`, "good");
    return r.id;
  }, [client, refresh, sel, toast]);

  const doInject = useCallback(async (tid: number, key: string, text: string) => {
    let target = tid;
    const w = worldRef.current!;
    if (sel.g !== w.timelines[tid].head) target = await doFork(sel.g, tid);  // edits happen at a head: fork first
    await client.inject(target, key, text);
    await refresh();
    const n = await stepLive(target, 200);
    const w2 = worldRef.current ?? (await client.world());
    const t = w2.timelines[target];
    toast(`${t.name}: injected ${key}, ran ${n} steps. ${t.cache_hits}/${t.llm_calls} LLM calls served from the recording. Status: ${t.status}.`, t.status === "done" ? "good" : undefined);
    return target;
  }, [client, doFork, refresh, sel.g, stepLive, toast]);

  const doSpeculate = useCallback(async (tid = sel.tid, g = sel.g, n = 8) => {
    const r = await client.speculate(tid, g, n);
    const w = await refresh();
    const best = w.timelines[r.ranked[0]];
    const ok = r.ranked.filter((id) => w.timelines[id].status === "done").length;
    setSel({ tid: best.id, g: best.head });
    toast(`Speculated ${n} sampled branches from step ${g}: ${ok}/${n} succeeded. Kept ${best.name}, pruned the rest.`, ok ? "good" : "bad");
  }, [client, refresh, sel, toast]);

  const doVerify = useCallback(async (tid = sel.tid) => {
    const r = await client.verify(tid);
    setVerify(r);
    setTab("verify");
    await refresh();
    if (r.ok) toast(`verify(${worldRef.current?.timelines[tid].name}): ${r.steps} steps replayed bit-exactly in ${r.ms.toFixed(2)} ms.`, "good");
    else toast(`verify: divergence at step ${r.step}. ${r.reason}`, "bad");
    return r;
  }, [client, refresh, sel.tid, toast]);

  const doTamper = useCallback(async (corrupt: boolean) => {
    const w = worldRef.current!;
    const owner = ownerOf(w, sel.tid, sel.g);
    const ok = corrupt ? await client.corrupt(owner, sel.g) : await client.tamper(owner, sel.g);
    if (!ok) {
      toast(corrupt ? "Nothing to corrupt at this step." : "This step has no recorded LLM response to tamper with. Pick a step with an LLM call.", "bad");
      return;
    }
    toast(corrupt ? `Flipped one bit in step ${sel.g}'s commit frame (CRC left stale).` : `Rewrote a byte of step ${sel.g}'s LLM response and fixed its CRC.`);
    await doVerify(owner);
  }, [client, doVerify, sel, toast]);

  const doKill = useCallback(async (actor: number) => {
    const w = worldRef.current!;
    let tid = sel.tid;
    if (sel.g !== w.timelines[tid].head) tid = await doFork(sel.g, tid);
    await client.kill(tid, actor);
    await stepLive(tid, 200);
    toast(`Chaos: killed ${ACTORS[actor]} on ${worldRef.current?.timelines[tid].name}. The supervisor restarted it from committed state.`);
  }, [client, doFork, sel, stepLive, toast]);

  // ---- playback -----------------------------------------------------------------------------
  useEffect(() => {
    if (!playing || !world) return;
    const t = world.timelines[sel.tid];
    const id = setTimeout(async () => {
      if (sel.g < t.head) setSel({ tid: sel.tid, g: sel.g + 1 });
      else if (t.status === "running" && !t.pruned) {
        const n = await client.step(sel.tid, 1);
        if (n.executed === 0) setPlaying(false);
        const w = await refresh();
        setSel({ tid: sel.tid, g: w.timelines[sel.tid].head });
      } else setPlaying(false);
    }, 420);
    return () => clearTimeout(id);
  }, [playing, sel, world, client, refresh]);

  // ---- keyboard -----------------------------------------------------------------------------
  useEffect(() => {
    const on = (e: KeyboardEvent) => {
      if (!world || !tl || injectOpen) return;
      const el = e.target as HTMLElement;
      if (el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.tagName === "SELECT")) return;
      const min = 0;
      if (e.key === " ") { e.preventDefault(); setPlaying((p) => !p); }
      else if (e.key === "ArrowRight" || e.key === "k") setSel((s) => ({ ...s, g: Math.min(tl.head, s.g + 1) }));
      else if (e.key === "ArrowLeft" || e.key === "j") setSel((s) => ({ ...s, g: Math.max(min, s.g - 1) }));
      else if (e.key === "Home") setSel((s) => ({ ...s, g: 0 }));
      else if (e.key === "End") setSel((s) => ({ ...s, g: tl.head }));
      else if (e.key === "f") run("fork", async () => void (await doFork()));
      else if (e.key === "v") run("verify", async () => void (await doVerify()));
      else if (e.key === "i") setInjectOpen(true);
      else if (e.key === "s") run("speculate", () => doSpeculate());
      else if (e.key === "]" || e.key === "[") {
        const ids = world.timelines.map((t) => t.id);
        const i = ids.indexOf(sel.tid) + (e.key === "]" ? 1 : -1);
        const next = world.timelines[(i + ids.length) % ids.length];
        setSel({ tid: next.id, g: Math.min(next.head, Math.max(sel.g, 0)) });
      }
    };
    addEventListener("keydown", on);
    return () => removeEventListener("keydown", on);
  }, [world, tl, sel, injectOpen, run, doFork, doVerify, doSpeculate]);

  // ---- story --------------------------------------------------------------------------------
  const mark = (n: number) => setChapters((c) => new Set(c).add(n));
  const findTag = (tid: number, tag: string) => world?.timelines[tid].steps.find((s) => s.tag === tag)?.g;
  const story = [
    {
      title: "The incident",
      text: "A planner, coder, tester and reviewer failed issue #42. Replay the recorded run.",
      cta: "Replay ▶",
      act: async () => { setSel({ tid: 0, g: 0 }); setPlaying(true); },
    },
    {
      title: "Root cause",
      text: "Jump to the first hallucinated API call. Step back once (←) to read the model's recorded response.",
      cta: "Find it",
      act: async () => { const g = findTag(0, "hallucination"); if (g !== undefined) { setSel({ tid: 0, g }); setTab("step"); } },
    },
    {
      title: "Fork & fix",
      text: "Fork at genesis, put the real API into the coder's memory, run. The planner's unchanged prompt replays from the recording; only changed prompts go live.",
      cta: "Fork + inject",
      act: async () => {
        setSel({ tid: 0, g: 0 });
        const id = await doFork(0, 0);
        await client.inject(id, "@coder/notes", HINT);
        await stepLive(id, 200);
        const t = (await client.world()).timelines[id];
        toast(`${t.name}: ${t.status}. ${t.cache_hits}/${t.llm_calls} LLM calls replayed from the recording (${t.tokens_saved} tokens saved).`, "good");
        setTab("diff");
      },
    },
    {
      title: "Speculate",
      text: "Fork 8 differently-sampled branches at genesis, run them all, keep the one the evaluator scores best.",
      cta: "Speculate ×8",
      act: async () => doSpeculate(0, 0, 8),
    },
    {
      title: "Trust, but verify",
      text: "Replay from the log alone and check digests at every step. Then tamper with a recorded response.",
      cta: "Verify + tamper",
      act: async () => {
        const r = await doVerify(0);
        if (!r.ok) return;
        const g = (findTag(0, "hallucination") ?? 4) - 1;
        setSel({ tid: 0, g });
        await client.tamper(0, g);
        toast(`Tampered with step ${g}'s LLM response (CRC fixed up). Re-verifying…`);
        await doVerify(0);
      },
    },
    {
      title: "Chaos",
      text: "Kill the coder mid-run. The supervisor restarts it from committed state; the run still verifies.",
      cta: "Kill coder",
      act: async () => {
        const id = await doFork(3, 0);
        await client.kill(id, 3);
        await stepLive(id, 200);
        await doVerify(id);
      },
    },
  ];

  if (fatal) return <div className="loading"><div className="callout bad">Failed to start the kernel: {fatal}</div></div>;
  if (!world || !tl) {
    return (
      <div className="loading">
        <div className="moon-spin"><Moon size={44} /></div>
        <div>Booting the WebAssembly kernel…</div>
      </div>
    );
  }

  const files = state?.entries ?? [];
  const selStep = world.timelines[ownerOf(world, sel.tid, sel.g)].steps.find((s) => s.g === sel.g);

  return (
    <>
      <main className="main">
        <section className="panel multiverse">
          <div className="panel-h">
            <h2>Timeline multiverse</h2>
            <span className="hint">one lane per timeline · one node per committed step · color = Merkle state digest (equal color ⇒ identical world)</span>
          </div>
          <div className="story" aria-label="Guided tour">
            {story.map((c, i) => (
              <div key={i} className={`chapter ${chapters.has(i) ? "done" : ""}`}>
                <span className="n">{String(i + 1).padStart(2, "0")}</span>
                <h3>{c.title}</h3>
                <p>{c.text}</p>
                <button className="btn" disabled={busy} onClick={() => run(c.title, async () => { await c.act(); mark(i); })}>{c.cta}</button>
              </div>
            ))}
          </div>
          <Multiverse world={world} sel={sel} onSelect={(s) => { setSel(s); setPlaying(false); }} />
          <div className="transport">
            <div className="group">
              <button className="btn icon" title="First step (Home)" onClick={() => setSel({ ...sel, g: 0 })}>⏮</button>
              <button className="btn icon" title="Previous step (←)" onClick={() => setSel({ ...sel, g: Math.max(0, sel.g - 1) })}>◀</button>
              <button className="btn primary icon" title="Play / pause (space)" onClick={() => setPlaying(!playing)}>{playing ? "❚❚" : "▶"}</button>
              <button className="btn icon" title="Next step (→)" onClick={() => setSel({ ...sel, g: Math.min(tl.head, sel.g + 1) })}>▶|</button>
              <button className="btn icon" title="Head (End)" onClick={() => setSel({ ...sel, g: tl.head })}>⏭</button>
            </div>
            <div className="scrub">
              <input type="range" min={0} max={tl.head} value={sel.g} onChange={(e) => { setPlaying(false); setSel({ ...sel, g: Number(e.target.value) }); }} aria-label="Scrub steps" />
              <span className="pos"><span style={{ color: laneColor(tl.id) }}>{tl.name}</span> {sel.g}/{tl.head}</span>
            </div>
            <div className="sep" />
            <div className="group">
              <button className="btn" disabled={busy} onClick={() => run("fork", async () => void (await doFork()))} title="Fork at the selected step (F)">⑂ Fork <kbd>F</kbd></button>
              <button className="btn" disabled={busy} onClick={() => setInjectOpen(true)} title="Edit agent memory at this step (I)">✎ Inject <kbd>I</kbd></button>
              <button className="btn" disabled={busy || tl.status !== "running" || sel.g !== tl.head || tl.pruned} onClick={() => run("step", async () => void (await stepLive(sel.tid, 1)))}>Step live</button>
              <button className="btn" disabled={busy} onClick={() => run("speculate", () => doSpeculate())} title="Speculate ×8 from here (S)">Speculate ×8 <kbd>S</kbd></button>
              <select disabled={busy} value="" onChange={(e) => { const a = Number(e.target.value); if (a) run("kill", () => doKill(a)); }} aria-label="Kill an actor">
                <option value="">ϟ Chaos: kill…</option>
                {[1, 2, 3, 4, 5].map((a) => <option key={a} value={a}>{ACTORS[a]}</option>)}
              </select>
              <button className="btn" disabled={busy} onClick={() => run("verify", async () => void (await doVerify()))} title="Verify (V)">✓ Verify <kbd>V</kbd></button>
            </div>
          </div>
          {selStep && (
            <div className="transport" style={{ paddingTop: 8, paddingBottom: 8, fontSize: 12.5 }}>
              <span className="chip" style={{ color: ACTOR_COLORS[selStep.actor] }}>{ACTORS[selStep.actor]}</span>
              <span style={{ flex: 1, minWidth: 0 }}>{selStep.label}</span>
              <span className="mono" style={{ color: "var(--text-3)" }}>{selStep.digest.slice(0, 12)}</span>
            </div>
          )}
        </section>

        <Inspector
          client={client} world={world} sel={sel} state={state} tab={tab} setTab={setTab} version={version} verify={verify}
          onVerify={() => run("verify", async () => void (await doVerify()))}
          onTamper={() => run("tamper", () => doTamper(false))}
          onCorrupt={() => run("corrupt", () => doTamper(true))}
          onExport={() => run("export", async () => {
            const bytes = await client.exportLog(sel.tid);
            download(`khonsu-${tl.name}.khn`, bytes);
            toast(`Exported ${tl.name}: ${fmtBytes(bytes.length)}, ${tl.log_frames} CRC'd frames.`);
          })}
          onImport={(f) => run("import", async () => {
            const r = await client.importLog(new Uint8Array(await f.arrayBuffer()));
            const w = await refresh();
            if (r.id >= 0) setSel({ tid: r.id, g: w.timelines[r.id].head });
            toast(r.error ? `Import stopped: ${r.error}` : `Imported ${f.name} by verified replay: all digests matched.`, r.error ? "bad" : "good");
          })}
          onGc={() => run("gc", async () => {
            const r = await client.gc();
            await refresh();
            toast(`GC: freed ${fmtInt(r.freed)} nodes in ${r.ms.toFixed(2)} ms (mark-sweep from retained roots).`);
          })}
        />

        <div className="lower">
          <PageMap client={client} world={world} sel={sel} files={files} version={version} />
          <section className="panel">
            <div className="panel-h">
              <h2>Causal trace</h2>
              <span className="hint">spans per agent over virtual time · hatched = served from the oracle cache · dashed = sandboxed tool</span>
            </div>
            <div className="panel-b"><Trace world={world} sel={sel} /></div>
            <div className="console" aria-live="polite">
              {consoleLines.length === 0 && <div><span className="t">--:--:--</span>kernel console</div>}
              {consoleLines.slice().reverse().map((l, i) => <div key={i}><span className="t">{l.t}</span>{l.m}</div>)}
            </div>
          </section>
        </div>
      </main>

      {injectOpen && (
        <InjectDialog
          onClose={() => setInjectOpen(false)}
          onSubmit={(key, text) => { setInjectOpen(false); run("inject", async () => void (await doInject(sel.tid, key, text))); }}
          atHead={sel.g === tl.head}
        />
      )}

      <div className="toast-stack">
        {toasts.map((t) => <div key={t.id} className={`toast ${t.kind ?? ""}`}>{t.msg}</div>)}
      </div>
    </>
  );
}

function InjectDialog({ onClose, onSubmit, atHead }: { onClose: () => void; onSubmit: (k: string, t: string) => void; atHead: boolean }) {
  const [key, setKey] = useState("@coder/notes");
  const [text, setText] = useState(HINT);
  return (
    <div style={{ position: "fixed", inset: 0, background: "rgba(3,5,10,.6)", display: "grid", placeItems: "center", zIndex: 60 }} onClick={onClose}>
      <div className="panel" style={{ width: "min(560px, calc(100vw - 32px))" }} onClick={(e) => e.stopPropagation()} role="dialog" aria-label="Inject a state edit">
        <div className="panel-h"><h2>Inject a state edit</h2></div>
        <div className="panel-b" style={{ display: "grid", gap: 10 }}>
          <div style={{ color: "var(--text-2)", fontSize: 12.5 }}>
            Writes a key into the world state as a recorded <code>UserEdit</code> step, then runs the timeline.
            {!atHead && " You're not at the head, so Khonsu forks first. History is never rewritten."}
          </div>
          <select value={key} onChange={(e) => setKey(e.target.value)}>
            <option value="@coder/notes">@coder/notes (the coder's scratch memory)</option>
            <option value="@planner/plan">@planner/plan</option>
            <option value="@researcher/notes">@researcher/notes</option>
          </select>
          <textarea rows={4} value={text} onChange={(e) => setText(e.target.value)} />
          <div style={{ display: "flex", gap: 8, justifyContent: "flex-end" }}>
            <button className="btn ghost" onClick={onClose}>Cancel</button>
            <button className="btn primary" onClick={() => onSubmit(key, text)}>{atHead ? "Inject & run" : "Fork, inject & run"}</button>
          </div>
        </div>
      </div>
    </div>
  );
}
