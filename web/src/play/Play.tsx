// Play: fork a running program. Lune programs run in the kernel worker; every VM
// state lives in the same persistent Merkle store as the agent recordings, so a
// fork is a commit of the dirty pages plus an O(1) root copy, and the child copies
// a 4 KiB page only when it first writes to it.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { PageHelp } from "../components/PageHelp";
import { kernel } from "../kernel/client";
import { fmtBytes, fmtInt, laneColor } from "../lib/ui";
import { PRESETS } from "./programs";
import type { ForkInfo, PageReport, SpecResult, VmState } from "./types";

const GAS = 400_000_000;
const SPEEDS = [
  { label: "0.1×", steps: 2_000 },
  { label: "1×", steps: 40_000 },
  { label: "10×", steps: 400_000 },
  { label: "max", steps: 4_000_000 },
];

interface Branch {
  id: number;
  parent: number;
  forkedAt: number; // parent's step count at the fork
  edit?: string;
  state: VmState;
}

export function Play() {
  const client = kernel();
  const [presetIx, setPresetIx] = useState(0);
  const [src, setSrc] = useState(PRESETS[0].src);
  const [compileErr, setCompileErr] = useState<{ error: string; line: number } | null>(null);
  const [branches, setBranches] = useState<Map<number, Branch>>(new Map());
  const [sel, setSel] = useState<number | null>(null);
  const [running, setRunning] = useState<Set<number>>(new Set());
  const [speed, setSpeed] = useState(1);
  const [lastFork, setLastFork] = useState<ForkInfo | null>(null);
  const [spec, setSpec] = useState<SpecResult | null>(null);
  const [specVar, setSpecVar] = useState(PRESETS[0].knob);
  const [specN, setSpecN] = useState(8);
  const [pages, setPages] = useState<PageReport | null>(null);
  const [fatal, setFatal] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const branchesRef = useRef(branches);
  branchesRef.current = branches;
  const runningRef = useRef(running);
  runningRef.current = running;
  const speedRef = useRef(speed);
  speedRef.current = speed;

  const upsert = useCallback((b: Partial<Branch> & { state: VmState }) => {
    setBranches((m) => {
      const next = new Map(m);
      const prev = next.get(b.state.id);
      next.set(b.state.id, {
        id: b.state.id,
        parent: b.parent ?? prev?.parent ?? b.state.parent,
        forkedAt: b.forkedAt ?? prev?.forkedAt ?? 0,
        edit: b.edit ?? prev?.edit,
        state: b.state,
      });
      return next;
    });
  }, []);

  const load = useCallback(
    async (code: string) => {
      setRunning(new Set());
      setBusy(true);
      try {
        for (const id of branchesRef.current.keys()) await client.luneFree(id);
        await client.luneGc();
        const r = await client.luneNew(code, GAS);
        if ("error" in r && !("id" in r)) {
          setCompileErr(r);
          setBranches(new Map());
          setSel(null);
          return;
        }
        const st = r as VmState;
        setCompileErr(null);
        setSpec(null);
        setLastFork(null);
        setBranches(new Map([[st.id, { id: st.id, parent: -1, forkedAt: 0, state: st }]]));
        setSel(st.id);
        setRunning(new Set([st.id]));
      } catch (e) {
        setFatal(e instanceof Error ? e.message : String(e));
      } finally {
        setBusy(false);
      }
    },
    [client],
  );

  useEffect(() => {
    void load(PRESETS[0].src);
  }, [load]);

  // The run loop: each frame gives every running branch one time slice in the worker.
  useEffect(() => {
    let raf = 0;
    let inflight = false;
    const tick = async () => {
      raf = requestAnimationFrame(tick);
      if (inflight || runningRef.current.size === 0) return;
      inflight = true;
      try {
        const ids = [...runningRef.current];
        const slice = Math.max(1, Math.floor(SPEEDS[speedRef.current].steps / ids.length));
        const done: number[] = [];
        for (const id of ids) {
          const st = await client.luneRun(id, slice);
          upsert({ state: st });
          if (st.status !== "running") done.push(id);
        }
        if (done.length)
          setRunning((s) => {
            const n = new Set(s);
            done.forEach((d) => n.delete(d));
            return n;
          });
      } catch (e) {
        setFatal(e instanceof Error ? e.message : String(e));
      } finally {
        inflight = false;
      }
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [client, upsert]);

  const cur = sel !== null ? branches.get(sel) : undefined;

  // Page map of the selected branch against its parent, refreshed a few times a second.
  const curId = cur?.id;
  const curSteps = cur?.state.steps ?? 0;
  const pageTick = Math.floor(curSteps / 200_000);
  useEffect(() => {
    if (curId === undefined) return;
    let live = true;
    client
      .lunePages(curId)
      .then((p) => live && setPages(p))
      .catch(() => live && setPages(null));
    return () => {
      live = false;
    };
  }, [client, curId, pageTick, cur?.state.status]);

  const toggleRun = (id: number) =>
    setRunning((s) => {
      const n = new Set(s);
      if (n.has(id)) n.delete(id);
      else if (branches.get(id)?.state.status === "running") n.add(id);
      return n;
    });

  const fork = async (id: number) => {
    const parent = branches.get(id);
    if (!parent) return;
    const f = await client.luneFork(id);
    const st = await client.luneState(f.id);
    setLastFork(f);
    upsert({ state: st, parent: id, forkedAt: st.steps });
    setSel(f.id);
    return f.id;
  };

  const setVar = async (id: number, name: string, raw: string) => {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    const st = await client.luneSet(id, name, Math.trunc(v));
    upsert({ state: st, edit: `${name} = ${Math.trunc(v)}` });
  };

  const speculate = async () => {
    if (!cur) return;
    setBusy(true);
    try {
      const base = Number(cur.state.vars.find(([k]) => k === specVar)?.[1] ?? 0);
      const r = await client.luneSpeculate(cur.id, specVar, base + 1, specN, 20_000_000);
      setSpec(r);
      for (const b of r.branches) {
        const st = await client.luneState(b.id);
        upsert({ state: st, parent: cur.id, forkedAt: cur.state.steps, edit: `${specVar} = ${b.value}` });
      }
    } catch (e) {
      setFatal(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  const numVars = cur?.state.vars.filter(([, v]) => !v.startsWith("array(")) ?? [];

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.target as HTMLElement)?.closest("textarea, input, select")) return;
      if (sel === null) return;
      if (e.key === " ") {
        e.preventDefault();
        toggleRun(sel);
      } else if (e.key === "f") {
        void fork(sel);
      }
    };
    addEventListener("keydown", onKey);
    return () => removeEventListener("keydown", onKey);
  });

  if (fatal)
    return (
      <div className="empty">
        The kernel failed: {fatal}. <button className="btn" onClick={() => location.reload()}>Reload</button>
      </div>
    );

  return (
    <div className="play">
      <section className="panel play-intro">
        <div className="panel-b">
          <p className="eyebrow">Play · fork a running program</p>
          <p className="lede small-lede">
            Lune is a tiny deterministic VM whose whole state (stack, variables, arrays) lives in Khonsu's persistent
            Merkle store. Press <kbd>F</kbd> while it runs: the fork commits the dirty pages, shares everything else, and
            both copies continue. Edit a variable in the fork to ask "what if?", or speculate a dozen futures at once.
          </p>
        </div>
      </section>

      <PageHelp id="play" title="Play" steps={[
        <>A program is already running. Watch its output on the right and its branch in the middle.</>,
        <>Press <b>⑂ Fork</b> or the <kbd>F</kbd> key while it runs. A second branch appears, sharing all its memory with the first.</>,
        <>Select the new branch, change a number in <b>Variables</b>, and press <b>▶ Run</b> to see a different future.</>,
        <>The page map shows the memory the fork actually copied (gold and turquoise); grey is still shared.</>,
        <>Try the <b>Gambler</b> preset and <b>Run all</b> to play eight futures from the same moment.</>,
      ]} />

      <div className="play-grid">
        <Editor
          src={src}
          setSrc={setSrc}
          err={compileErr}
          line={cur?.state.line}
          presetIx={presetIx}
          onPreset={(i) => {
            setPresetIx(i);
            setSrc(PRESETS[i].src);
            setSpecVar(PRESETS[i].knob);
            void load(PRESETS[i].src);
          }}
          onRun={() => void load(src)}
          busy={busy}
        />

        <section className="panel play-tree" aria-label="Branches">
          <div className="panel-h">
            <h2>Branches</h2>
            <span className="spacer" />
            <div className="seg" role="radiogroup" aria-label="Speed">
              {SPEEDS.map((s, i) => (
                <button key={s.label} role="radio" aria-checked={speed === i} className={speed === i ? "on" : ""}
                  onClick={() => setSpeed(i)}>{s.label}</button>
              ))}
            </div>
          </div>
          <div className="panel-b">
            <Tree branches={branches} sel={sel} onSel={setSel} running={running} />
            {cur && (
              <div className="transport play-transport">
                <button className="btn primary" onClick={() => toggleRun(cur.id)} disabled={cur.state.status !== "running"}
                  aria-label={running.has(cur.id) ? "Pause" : "Run"}>
                  {running.has(cur.id) ? "❚❚ Pause" : "▶ Run"}
                </button>
                <button className="btn" onClick={() => void client.luneRun(cur.id, 1).then((st) => upsert({ state: st }))}
                  disabled={cur.state.status !== "running"}>Step</button>
                <button className="btn gold" onClick={() => void fork(cur.id)} disabled={busy}>⑂ Fork</button>
                <span className="spacer" />
                {lastFork && (
                  <span className="chip mono" title="The first fork commits the parent's dirty pages; forking an already committed VM is a root copy.">
                    fork: {lastFork.dirty_pages_committed} dirty pages committed · then {lastFork.fork_committed_us.toFixed(1)} µs/fork
                  </span>
                )}
              </div>
            )}
            {cur && (
              <div className="spec">
                <span className="muted small">Speculate</span>
                <input type="number" min={1} max={64} value={specN} onChange={(e) => setSpecN(Math.max(1, Math.min(64, Number(e.target.value) || 1)))}
                  aria-label="Number of branches" style={{ width: 56 }} />
                <span className="muted small">futures, each with</span>
                <select value={specVar} onChange={(e) => setSpecVar(e.target.value)} aria-label="Variable to vary">
                  {numVars.map(([k]) => <option key={k} value={k}>{k}</option>)}
                </select>
                <span className="muted small">+1, +2, …</span>
                <button className="btn" onClick={() => void speculate()} disabled={busy || !numVars.some(([k]) => k === specVar)}>
                  Run all
                </button>
              </div>
            )}
            {spec && <SpecTable spec={spec} onSel={setSel} knob={specVar} />}
          </div>
        </section>

        {cur && (
          <section className="panel play-inspect" aria-label="Selected branch">
            <div className="panel-h">
              <h2>
                <i className="dot" style={{ background: laneColor(cur.id) }} /> Branch #{cur.id}
              </h2>
              <span className="spacer" />
              <span className={`chip status-${cur.state.status.replace(/ /g, "-")}`}>{cur.state.status}</span>
            </div>
            <div className="panel-b play-inspect-b">
              <div className="kv">
                <span className="muted">steps</span><span className="mono">{fmtInt(cur.state.steps)}</span>
                <span className="muted">line</span><span className="mono">{cur.state.line}</span>
                {cur.parent >= 0 && (<><span className="muted">forked from</span><span className="mono">#{cur.parent} at step {fmtInt(cur.forkedAt)}</span></>)}
                {cur.edit && (<><span className="muted">what-if</span><span className="mono gold-t">{cur.edit}</span></>)}
                <span className="muted">pages copied since fork</span><span className="mono">{fmtInt(cur.state.pages_copied)}</span>
                {cur.state.error && (<><span className="muted">error</span><span className="mono bad-t">{cur.state.error}</span></>)}
              </div>
              <div className="section-t">Variables {cur.state.status === "running" && <span className="muted">(edit to fork a what-if)</span>}</div>
              <div className="vars">
                {cur.state.vars.map(([k, v]) => (
                  <VarRow key={k} name={k} value={v} editable={!v.startsWith("array(") && !running.has(cur.id)}
                    onSet={(val) => void setVar(cur.id, k, val)} />
                ))}
              </div>
              <div className="section-t">Output</div>
              <pre className="vm-out" aria-live="polite">{cur.state.output.slice(-60).join("\n") || " "}</pre>
              <div className="section-t">Copy-on-write pages {cur.parent >= 0 ? "vs the fork point" : ""}</div>
              <Pages report={pages} isFork={cur.parent >= 0} />
            </div>
          </section>
        )}
      </div>
    </div>
  );
}

function Editor({ src, setSrc, err, line, presetIx, onPreset, onRun, busy }: {
  src: string;
  setSrc: (s: string) => void;
  err: { error: string; line: number } | null;
  line?: number;
  presetIx: number;
  onPreset: (i: number) => void;
  onRun: () => void;
  busy: boolean;
}) {
  const gutter = useRef<HTMLDivElement>(null);
  const lines = src.split("\n").length;
  return (
    <section className="panel play-editor" aria-label="Program">
      <div className="panel-h">
        <h2>Program</h2>
        <span className="spacer" />
        <select value={presetIx} onChange={(e) => onPreset(Number(e.target.value))} aria-label="Preset program">
          {PRESETS.map((p, i) => <option key={p.name} value={i}>{p.name}</option>)}
        </select>
        <button className="btn primary" onClick={onRun} disabled={busy}>Compile &amp; run</button>
      </div>
      <div className="panel-b">
        <p className="muted small" style={{ marginTop: 0 }}>{PRESETS[presetIx].blurb}</p>
        <div className="code">
          <div className="gutter" ref={gutter} aria-hidden>
            {Array.from({ length: lines }, (_, i) => (
              <div key={i} className={i + 1 === err?.line ? "err" : i + 1 === line ? "cur" : ""}>{i + 1}</div>
            ))}
          </div>
          <textarea value={src} spellCheck={false} aria-label="Lune source"
            onChange={(e) => setSrc(e.target.value)}
            onScroll={(e) => gutter.current && (gutter.current.scrollTop = e.currentTarget.scrollTop)}
            onKeyDown={(e) => {
              if ((e.ctrlKey || e.metaKey) && e.key === "Enter") onRun();
            }} />
        </div>
        {err && <div className="callout bad-callout">line {err.line}: {err.error}</div>}
        <details className="muted small lang">
          <summary>Language</summary>
          <code>let x = 1</code> · <code>x = x + 1</code> · <code>while c {"{ … }"}</code> · <code>if c {"{ … }"} else {"{ … }"}</code> ·{" "}
          <code>print x</code> · <code>print "text"</code> · <code>array(n)</code> · <code>len(a)</code> · <code>a[i]</code> ·{" "}
          <code>hash(x)</code> · <code>and or not</code> · <code># comment</code>. 64-bit integers; every instruction costs 1 gas.
        </details>
      </div>
    </section>
  );
}

function Tree({ branches, sel, onSel, running }: {
  branches: Map<number, Branch>;
  sel: number | null;
  onSel: (id: number) => void;
  running: Set<number>;
}) {
  // depth-first order so children sit under their parent
  const rows = useMemo(() => {
    const kids = new Map<number, Branch[]>();
    for (const b of branches.values()) {
      const k = kids.get(b.parent) ?? [];
      k.push(b);
      kids.set(b.parent, k);
    }
    const out: { b: Branch; depth: number }[] = [];
    const walk = (p: number, d: number) => {
      for (const b of (kids.get(p) ?? []).sort((x, y) => x.id - y.id)) {
        out.push({ b, depth: d });
        walk(b.id, d + 1);
      }
    };
    walk(-1, 0);
    return out;
  }, [branches]);
  const maxSteps = Math.max(1, ...[...branches.values()].map((b) => b.state.steps));
  return (
    <div className="tree" role="listbox" aria-label="Branches">
      {rows.map(({ b, depth }) => (
        <button key={b.id} role="option" aria-selected={sel === b.id} className={`tree-row ${sel === b.id ? "sel" : ""}`}
          onClick={() => onSel(b.id)} style={{ paddingLeft: 8 + depth * 18 }}>
          <i className="dot" style={{ background: laneColor(b.id), boxShadow: running.has(b.id) ? `0 0 8px ${laneColor(b.id)}` : undefined }} />
          <span className="mono">#{b.id}</span>
          <span className="bar" aria-hidden>
            {b.parent >= 0 && <span className="bar-inherited" style={{ width: `${(b.forkedAt / maxSteps) * 100}%` }} />}
            <span className="bar-fill" style={{ width: `${(b.state.steps / maxSteps) * 100}%`, background: laneColor(b.id) }} />
          </span>
          <span className="mono small tree-last">{b.edit ? `${b.edit} → ` : ""}{b.state.output.at(-1) ?? ""}</span>
          <span className={`chip tiny status-${b.state.status.replace(/ /g, "-")}`}>{running.has(b.id) ? "▶" : b.state.status}</span>
        </button>
      ))}
    </div>
  );
}

function SpecTable({ spec, onSel, knob }: { spec: SpecResult; onSel: (id: number) => void; knob: string }) {
  const nums = spec.branches.map((b) => Number(b.last)).filter(Number.isFinite);
  const best = nums.length ? Math.max(...nums) : NaN;
  return (
    <div className="spec-table">
      <div className="muted small">
        {spec.branches.length} futures ran in {spec.ms.toFixed(0)} ms, each from the same instant, sharing its pages.
      </div>
      <div className="spec-rows">
        {spec.branches.map((b) => (
          <button key={b.id} className="spec-row" onClick={() => onSel(b.id)}>
            <span className="mono">#{b.id}</span>
            <span className="mono">{knob} = {b.value}</span>
            <span className={`mono ${Number(b.last) === best ? "gold-t" : ""}`}>{b.last || "—"}</span>
            <span className="mono muted">{fmtInt(b.pages_copied)} pg copied</span>
          </button>
        ))}
      </div>
    </div>
  );
}

function VarRow({ name, value, editable, onSet }: { name: string; value: string; editable: boolean; onSet: (v: string) => void }) {
  const [draft, setDraft] = useState<string | null>(null);
  return (
    <label className="var">
      <span className="mono">{name}</span>
      {editable ? (
        <input className="mono" value={draft ?? value} aria-label={`Set ${name}`}
          onChange={(e) => setDraft(e.target.value)}
          onBlur={() => {
            if (draft !== null && draft !== value) onSet(draft);
            setDraft(null);
          }}
          onKeyDown={(e) => e.key === "Enter" && (e.target as HTMLInputElement).blur()} />
      ) : (
        <span className="mono muted">{value}</span>
      )}
    </label>
  );
}

function Pages({ report, isFork }: { report: PageReport | null; isFork: boolean }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const arr = report?.arrays.reduce<PageReport["arrays"][number] | null>((a, b) => (!a || b.pages > a.pages ? b : a), null);
  useEffect(() => {
    const c = canvas.current;
    if (!c || !arr) return;
    const cell = arr.pages > 4096 ? 3 : arr.pages > 1024 ? 5 : 8;
    const cols = Math.max(1, Math.floor(c.clientWidth / cell));
    const rows = Math.ceil(arr.pages / cols);
    const dpr = devicePixelRatio || 1;
    c.width = cols * cell * dpr;
    c.height = rows * cell * dpr;
    c.style.height = `${rows * cell}px`;
    const g = c.getContext("2d")!;
    g.scale(dpr, dpr);
    const css = getComputedStyle(c);
    const v = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const color: Record<string, string> = {
      s: v("--line-2", "#2a3d6b"),
      c: v("--gold", "#e9b949"),
      n: v("--faience", "#3cc8b4"),
      ".": "rgba(255,255,255,.05)",
    };
    for (let i = 0; i < arr.pages; i++) {
      g.fillStyle = color[arr.map[i]] ?? color["."];
      g.fillRect((i % cols) * cell, Math.floor(i / cols) * cell, cell - 1, cell - 1);
    }
  }, [arr]);
  if (!report || !arr) return <div className="muted small">No arrays yet.</div>;
  const untouched = arr.pages - arr.shared - arr.copied - arr.new;
  return (
    <div className="vm-pages">
      <canvas ref={canvas} className="pagecanvas" role="img"
        aria-label={`${arr.pages} pages: ${arr.shared} shared, ${arr.copied} copied, ${arr.new} new`} />
      <div className="legend">
        {isFork && <span><i className="dot" style={{ background: "var(--line-2)" }} /> shared {fmtInt(arr.shared)}</span>}
        {isFork && <span><i className="dot" style={{ background: "var(--gold)" }} /> copied {fmtInt(arr.copied)}</span>}
        <span><i className="dot" style={{ background: "var(--faience)" }} /> {isFork ? "new" : "written"} {fmtInt(arr.new)}</span>
        <span><i className="dot" style={{ background: "rgba(255,255,255,.08)" }} /> untouched {fmtInt(untouched)}</span>
        <span className="mono">array({fmtInt(arr.length)}) · {fmtBytes(arr.length * 8)}</span>
      </div>
      {isFork && (
        <div className="muted small">
          {arr.copied + arr.new === 0
            ? "Every page is physically shared with the fork point: this branch has copied zero bytes."
            : `Since the fork this branch copied ${fmtInt(arr.copied)} of ${fmtInt(arr.shared + arr.copied)} shared pages (${fmtBytes(
                arr.copied * 4096,
              )}) and wrote ${fmtInt(arr.new)} new ones; the other ${fmtInt(arr.shared)} are the fork point's own nodes.`}
        </div>
      )}
    </div>
  );
}
