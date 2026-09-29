import { useEffect, useMemo, useState } from "react";
import type { KernelClient } from "../kernel/client";
import type { DiffChange, Frame, StateInfo, VerifyResult, World } from "../kernel/types";
import { ACTORS } from "../kernel/types";
import { ACTOR_COLORS, TAGS, fmtBytes, fmtVtime, laneColor, lineDiff } from "../lib/ui";
import type { Sel } from "./Multiverse";

export type Tab = "step" | "state" | "diff" | "verify";

/** Timeline that recorded global step g (a fork's base step belongs to its parent). */
export function ownerOf(world: World, tid: number, g: number): number {
  let t = world.timelines[tid];
  while (t && t.parent >= 0 && g <= t.base) t = world.timelines[t.parent];
  return t ? t.id : tid;
}

interface Props {
  client: KernelClient;
  world: World;
  sel: Sel;
  state: StateInfo | null;
  tab: Tab;
  setTab: (t: Tab) => void;
  version: number;
  verify: VerifyResult | null;
  onVerify: () => void;
  onTamper: () => void;
  onCorrupt: () => void;
  onExport: () => void;
  onImport: (f: File) => void;
  onGc: () => void;
}

export function Inspector(p: Props) {
  const tabs: [Tab, string][] = [["step", "Step"], ["state", "State"], ["diff", "Diff"], ["verify", "Verify"]];
  return (
    <div className="panel inspector">
      <div className="tabs" role="tablist">
        {tabs.map(([k, label]) => (
          <button key={k} role="tab" aria-selected={p.tab === k} className={`tab ${p.tab === k ? "active" : ""}`} onClick={() => p.setTab(k)}>
            {label}
          </button>
        ))}
      </div>
      <div className="scroll panel-b">
        {p.tab === "step" && <StepTab {...p} />}
        {p.tab === "state" && <StateTab {...p} />}
        {p.tab === "diff" && <DiffTab {...p} />}
        {p.tab === "verify" && <VerifyTab {...p} />}
      </div>
    </div>
  );
}

// ---- Step ---------------------------------------------------------------------------------------

function StepTab({ client, world, sel, version }: Props) {
  const owner = ownerOf(world, sel.tid, sel.g);
  const t = world.timelines[owner];
  const step = t.steps.find((s) => s.g === sel.g);
  const [frames, setFrames] = useState<Frame[]>([]);
  useEffect(() => {
    let live = true;
    client.frames(owner, sel.g).then((r) => live && setFrames(r.frames ?? []));
    return () => {
      live = false;
    };
  }, [client, owner, sel.g, version]);
  if (!step) return null;
  const tag = TAGS[step.tag];
  return (
    <div>
      <div style={{ display: "flex", alignItems: "center", gap: 8, flexWrap: "wrap" }}>
        <span className="chip" style={{ color: laneColor(owner) }}>{t.name}</span>
        <span className="chip accent mono">step {step.g}</span>
        <span className="chip" style={{ color: ACTOR_COLORS[step.actor] }}>{ACTORS[step.actor]}</span>
        {tag && <span className="chip" style={{ color: tag.color }}>{tag.glyph} {tag.text}</span>}
      </div>
      <h3 style={{ margin: "10px 0 8px", fontSize: 14, fontWeight: 600, lineHeight: 1.35 }}>{step.label}</h3>
      {owner !== sel.tid && (
        <div className="callout" style={{ marginBottom: 10, fontSize: 12 }}>
          Inherited from <b>{t.name}</b>: this step predates the fork, so both timelines share it (same root handle).
        </div>
      )}
      <div className="kv">
        <span className="k">virtual time</span><span className="v">{fmtVtime(step.vtime)}</span>
        <span className="k">state digest</span><span className="v">{step.digest}</span>
        <span className="k">log frames</span><span className="v">lsn {step.lsn0} … {step.lsn1 - 1}</span>
        <span className="k">tokens</span><span className="v">{step.tokens}{step.saved > 0 ? ` (${step.saved} served from cache)` : ""}</span>
        <span className="k">host time</span><span className="v">{step.wall_us.toFixed(1)} µs</span>
      </div>
      <div className="section-t">Event-log frames for this step</div>
      {frames.map((f) => <FrameView key={f.lsn} f={f} />)}
    </div>
  );
}

function FrameView({ f }: { f: Frame }) {
  const [open, setOpen] = useState(f.type === "OracleResponse");
  const color =
    f.type === "OracleRequest" ? "#a78bfa" : f.type === "OracleResponse" ? "#7fdbca" : f.type === "StepCommit" ? "#f5c26b" : "#8a94a7";
  const expandable = f.type === "OracleRequest" || f.type === "OracleResponse";
  return (
    <div className="frame">
      <div className="frame-h" onClick={() => expandable && setOpen(!open)} style={{ cursor: expandable ? "pointer" : "default" }}>
        <span className="ty" style={{ color }}>{f.type}</span>
        <span className="mono" style={{ color: "var(--text-3)" }}>lsn {f.lsn}</span>
        <span className="mono" style={{ color: "var(--text-3)" }}>{f.bytes} B</span>
        <span className="spacer" style={{ flex: 1 }} />
        {f.cached !== undefined && (f.cached ? <span className="chip teal">cache hit</span> : <span className="chip warn">live call</span>)}
        <span className={`chip ${f.crc_ok ? "good" : "bad"}`} title={`crc32c ${Number(f.crc).toString(16)}`}>
          {f.crc_ok ? "CRC ✓" : "CRC ✗"}
        </span>
      </div>
      {f.type === "StepCommit" && (
        <div className="frame-b kv">
          <span className="k">state</span><span className="v">{f.state}</span>
          <span className="k">hash chain</span><span className="v">{f.chain}</span>
        </div>
      )}
      {f.type === "Fork" && (
        <div className="frame-b kv">
          <span className="k">parent</span><span className="v">timeline {f.parent} @ step {f.step}</span>
          <span className="k">root</span><span className="v">{f.state}</span>
        </div>
      )}
      {expandable && open && (
        <div className="frame-b">
          <div className="kv" style={{ marginBottom: 8 }}>
            <span className="k">request hash</span><span className="v">{f.hash}</span>
            {f.role && (<><span className="k">role · sample</span><span className="v">{f.role} · {f.sample}</span></>)}
            {f.latency_us !== undefined && (<><span className="k">latency · tokens</span><span className="v">{fmtVtime(f.latency_us)} · {f.prompt_tokens}+{f.completion_tokens}</span></>)}
          </div>
          <pre className="code">{f.prompt ?? f.text}</pre>
        </div>
      )}
    </div>
  );
}

// ---- State --------------------------------------------------------------------------------------

function StateTab({ client, sel, state, version }: Props) {
  const [open, setOpen] = useState<string | null>("/repo/src/user.ts");
  const [text, setText] = useState<string | null>(null);
  useEffect(() => {
    if (!open || !state?.entries.find((e) => e.key === open && e.type === "file")) {
      setText(null);
      return;
    }
    let live = true;
    client.file(sel.tid, sel.g, open).then((r) => live && setText(r.text));
    return () => {
      live = false;
    };
  }, [client, sel.tid, sel.g, open, state, version]);
  if (!state) return <div style={{ color: "var(--text-3)" }}>Loading…</div>;
  const groups: [string, (k: string) => boolean][] = [
    ["Virtual filesystem", (k) => k.startsWith("/")],
    ["Agent memory", (k) => k.startsWith("@")],
    ["Kernel", (k) => k.startsWith("sys/")],
  ];
  const openEntry = state.entries.find((e) => e.key === open);
  return (
    <div>
      <div className="kv">
        <span className="k">root digest</span><span className="v">{state.root}</span>
      </div>
      {groups.map(([title, f]) => (
        <div key={title}>
          <div className="section-t">{title}</div>
          {state.entries.filter((e) => f(e.key)).map((e) => (
            <div key={e.key} className={`entry ${open === e.key ? "sel" : ""}`} onClick={() => setOpen(open === e.key ? null : e.key)}>
              <span className="key">{e.key}</span>
              <span className="meta">{e.type === "file" ? `${fmtBytes(e.size)} · ${e.pages}pg` : fmtBytes(e.size)} · {e.digest.slice(0, 6)}</span>
            </div>
          ))}
        </div>
      ))}
      {openEntry && (
        <div style={{ marginTop: 10 }}>
          <div className="section-t">{openEntry.key}</div>
          <pre className="code" style={{ border: "1px solid var(--line)", borderRadius: 8, padding: 10 }}>
            {openEntry.type === "file" ? text ?? "…" : openEntry.preview}
          </pre>
        </div>
      )}
      <div className="section-t">Scheduler queue (part of the state, so it forks too)</div>
      {state.pending.length === 0 && <div style={{ color: "var(--text-3)" }}>empty: timeline is idle</div>}
      {state.pending.map((e) => (
        <div key={e.seq} className="entry" style={{ cursor: "default" }}>
          <span className="key">
            <span style={{ color: ACTOR_COLORS[e.to] }}>{ACTORS[e.to]}</span> ← {e.topic}
          </span>
          <span className="meta">t={fmtVtime(e.vtime)} · #{e.seq}</span>
        </div>
      ))}
    </div>
  );
}

// ---- Diff ---------------------------------------------------------------------------------------

function DiffTab({ client, world, sel, version }: Props) {
  const t = world.timelines[sel.tid];
  const options = useMemo(() => {
    const o: { label: string; tid: number; g: number }[] = [];
    if (t.parent >= 0) o.push({ label: `fork point (${world.timelines[t.parent].name} @ ${t.base})`, tid: t.parent, g: t.base });
    if (sel.g > 0) o.push({ label: "previous step", tid: sel.tid, g: sel.g - 1 });
    for (const other of world.timelines)
      if (other.id !== sel.tid && !other.pruned) o.push({ label: `${other.name} head (step ${other.head})`, tid: other.id, g: other.head });
    return o;
  }, [world, sel, t]);
  const [pick, setPick] = useState(0);
  const base = options[Math.min(pick, options.length - 1)];
  const [changes, setChanges] = useState<DiffChange[] | null>(null);
  useEffect(() => {
    if (!base) return;
    let live = true;
    client.diff(base.tid, base.g, sel.tid, sel.g).then((r) => live && setChanges(r.changes));
    return () => {
      live = false;
    };
  }, [client, base?.tid, base?.g, sel.tid, sel.g, version]);
  if (!base) return <div style={{ color: "var(--text-3)" }}>Genesis has nothing to diff against.</div>;
  return (
    <div>
      <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
        <span style={{ color: "var(--text-3)" }}>compare with</span>
        <select value={pick} onChange={(e) => setPick(Number(e.target.value))} style={{ flex: 1 }}>
          {options.map((o, i) => <option key={i} value={i}>{o.label}</option>)}
        </select>
      </div>
      <div style={{ color: "var(--text-3)", fontSize: 11.5, margin: "8px 0" }}>
        Merkle diff: identical subtrees are skipped by handle or digest, so cost is O(changes), not O(state).
      </div>
      {changes?.length === 0 && <div className="callout good">States are identical (equal Merkle roots).</div>}
      {changes?.map((c) => (
        <div key={c.key} style={{ marginBottom: 12 }}>
          <div style={{ display: "flex", gap: 8, alignItems: "center", margin: "6px 0" }}>
            <span className={`chip ${c.kind === "added" ? "good" : c.kind === "removed" ? "bad" : "warn"}`}>{c.kind}</span>
            <span className="mono">{c.key}</span>
          </div>
          {c.before !== undefined && c.after !== undefined && (
            <div className="difflines">
              {lineDiff(c.before, c.after).map((l, i) => (
                <div key={i} className={l.t === "+" ? "add" : l.t === "-" ? "del" : "ctx"}>{l.t} {l.s}</div>
              ))}
            </div>
          )}
        </div>
      ))}
    </div>
  );
}

// ---- Verify -------------------------------------------------------------------------------------

function VerifyTab({ world, sel, verify, onVerify, onTamper, onCorrupt, onExport, onImport, onGc }: Props) {
  const t = world.timelines[sel.tid];
  return (
    <div style={{ display: "grid", gap: 12 }}>
      <div className="prose" style={{ fontSize: 12.5 }}>
        <b>verify()</b> re-executes <b>{t.name}</b> from its base using only the event log: recorded oracle responses are
        served back and every request hash is checked. The Merkle digest is compared at each commit and the hash chain over
        frame CRCs is recomputed.
      </div>
      <button className="btn primary" onClick={onVerify}>Verify {t.name} ({t.steps.length - 1} steps)</button>
      {verify && (
        <div className={`callout ${verify.ok ? "good" : "bad"}`}>
          {verify.ok ? (
            <>✓ <b>Bit-exact replay.</b> {verify.steps} steps re-executed and every state digest matched ({verify.ms.toFixed(2)} ms).</>
          ) : (
            <>
              ✕ <b>Divergence at step {verify.step}.</b> {verify.reason}
              {verify.diff.length > 0 && (
                <div className="mono" style={{ marginTop: 6, fontSize: 11.5 }}>differs: {verify.diff.join(", ")}</div>
              )}
            </>
          )}
        </div>
      )}
      <div className="section-t">Adversarial checks (on the selected step)</div>
      <div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
        <button className="btn danger" onClick={onTamper} disabled={sel.g <= t.base}>Tamper oracle response</button>
        <button className="btn danger" onClick={onCorrupt} disabled={sel.g <= t.base}>Flip a bit (bit-rot)</button>
      </div>
      <div style={{ color: "var(--text-3)", fontSize: 11.5 }}>
        <b>Tamper</b> edits a recorded LLM response <i>and fixes its CRC</i>, like an adversary would. The CRC can't catch it,
        but replay produces a different state and the digest check does. <b>Bit-rot</b> leaves the CRC stale, so CRC32C catches it.
      </div>
      <div className="section-t">Recorded traces</div>
      <div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
        <button className="btn" onClick={onExport} disabled={t.parent >= 0}>Export .khn</button>
        <label className="btn">
          Import .khn
          <input type="file" accept=".khn" hidden onChange={(e) => e.target.files?.[0] && onImport(e.target.files[0])} />
        </label>
        <button className="btn" onClick={onGc}>Run GC</button>
      </div>
      <div style={{ color: "var(--text-3)", fontSize: 11.5 }}>
        Import rebuilds the timeline by <i>verified replay</i> in this kernel, whose host entropy differs, so matching digests prove
        that the log alone determines the run. GC mark-sweeps from retained roots and releases pruned branches' history.
      </div>
    </div>
  );
}
