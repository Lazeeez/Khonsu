import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { PageHelp } from "../components/PageHelp";
import { kernel } from "../kernel/client";
import { fmtBytes, fmtVtime, lineDiff } from "../lib/ui";
import { MoonDial, type DialTick } from "./MoonDial";
import { importOtel, isOtel } from "./otel";
import type { CallView, ExchangeInfo, FileChange, FileEntry, LoadedRun, RunDiffView } from "./types";

const BASE = import.meta.env.BASE_URL;

interface Sample { file: string; title: string; blurb: string }

// ---------------------------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------------------------

async function loadBytes(name: string, bytes: Uint8Array): Promise<LoadedRun> {
  const head = new TextDecoder().decode(bytes.slice(0, 8));
  if (head.startsWith("KHNLOG")) {
    const view = await kernel().recLoad(bytes);
    return { name, source: "khn", view, handle: view.handle };
  }
  let json: unknown;
  try {
    json = JSON.parse(new TextDecoder().decode(bytes));
  } catch {
    throw new Error(`${name}: not a Khonsu recording (.khn) or an OpenTelemetry JSON trace`);
  }
  if (!isOtel(json)) throw new Error(`${name}: JSON is not an OTLP trace (expected "resourceSpans")`);
  return importOtel(json, name);
}

async function fetchRun(url: string, name: string): Promise<LoadedRun> {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${name}: HTTP ${r.status}`);
  return loadBytes(name, new Uint8Array(await r.arrayBuffer()));
}

// ---------------------------------------------------------------------------------------------
// small pieces
// ---------------------------------------------------------------------------------------------

function sseText(body: string): { text: string; tools: string[] } {
  let text = "";
  const tools: string[] = [];
  for (const line of body.split("\n")) {
    const l = line.trim();
    if (!l.startsWith("data:")) continue;
    const payload = l.slice(5).trim();
    if (payload === "[DONE]") continue;
    try {
      const ev = JSON.parse(payload);
      const d = ev.choices?.[0]?.delta;
      if (d?.content) text += d.content;
      for (const tc of d?.tool_calls ?? []) if (tc.function?.name) tools.push(tc.function.name);
    } catch {
      /* ignore partial lines */
    }
  }
  return { text, tools };
}

function contentText(c: unknown): string {
  if (typeof c === "string") return c;
  if (Array.isArray(c)) return c.map((p: any) => p?.text ?? "").join("");
  return c == null ? "" : JSON.stringify(c, null, 2);
}

function Chip({ children, tone }: { children: React.ReactNode; tone?: "good" | "bad" | "warn" | "gold" | "teal" }) {
  return <span className={`chip ${tone ?? ""}`}>{children}</span>;
}

function DiffBlock({ before, after }: { before: string; after: string }) {
  const lines = useMemo(() => lineDiff(before, after), [before, after]);
  return (
    <div className="difflines">
      {lines.map((l, i) => (
        <div key={i} className={l.t === "+" ? "add" : l.t === "-" ? "del" : "ctx"}>{l.t} {l.s}</div>
      ))}
    </div>
  );
}

// ---------------------------------------------------------------------------------------------
// call detail
// ---------------------------------------------------------------------------------------------

function CallDetail({ run, index }: { run: LoadedRun; index: number }) {
  const ex = run.view.exchanges[index];
  const [call, setCall] = useState<CallView | null>(null);
  const [raw, setRaw] = useState(false);
  useEffect(() => {
    let live = true;
    setCall(null);
    if (run.calls) setCall(run.calls[index]);
    else if (run.handle !== undefined) kernel().recCall(run.handle, index).then((c) => live && setCall(c));
    return () => {
      live = false;
    };
  }, [run, index]);
  if (!ex) return <div className="empty">No calls in this run.</div>;

  const header = (
    <div className="call-head">
      <Chip tone={ex.tool ? "teal" : "gold"}>{ex.tool ? "tool" : "model"}</Chip>
      <span className="mono">#{ex.seq}</span>
      <b>{ex.tool ? ex.tool_calls : ex.model}</b>
      {ex.latency_us !== undefined && <span className="muted mono">{fmtVtime(ex.latency_us)}</span>}
      {ex.streamed && <Chip>stream</Chip>}
      {ex.from_recording && <Chip tone="teal">replayed</Chip>}
      {!ex.tool && ex.prompt_tokens + ex.completion_tokens > 0 && (
        <span className="muted mono">{ex.prompt_tokens}+{ex.completion_tokens} tok</span>
      )}
      <span className="spacer" />
      <button className="btn ghost small" onClick={() => setRaw(!raw)}>{raw ? "Formatted" : "Raw JSON"}</button>
    </div>
  );
  if (!call) return <div className="call">{header}<div className="muted">Loading…</div></div>;

  if (raw)
    return (
      <div className="call">
        {header}
        <div className="section-t">request</div>
        <pre className="code">{JSON.stringify(call.request, null, 2)}</pre>
        <div className="section-t">response</div>
        <pre className="code">{typeof call.response?.body === "string" ? call.response.body : JSON.stringify(call.response?.body, null, 2)}</pre>
      </div>
    );

  if (ex.tool) {
    const body = call.response?.body as any;
    return (
      <div className="call">
        {header}
        <div className="section-t">arguments</div>
        <pre className="code">{JSON.stringify(call.request?.args ?? call.request, null, 2)}</pre>
        <div className="section-t">{body?.ok === false ? "raised" : "returned"}</div>
        <pre className={`code ${body?.ok === false ? "bad" : ""}`}>
          {body?.ok === false ? `${body.error?.type}: ${body.error?.message}` : JSON.stringify(body?.result, null, 2)}
        </pre>
        <p className="muted small">
          Recorded by <code>@khonsu.tool</code>. On replay this result is returned without re-running the tool, so side effects don't repeat.
        </p>
      </div>
    );
  }

  const messages: any[] = call.request?.messages ?? (call.request?.prompt ? [{ role: "user", content: call.request.prompt }] : []);
  let outText = "";
  let outTools: string[] = [];
  const body = call.response?.body;
  if (call.response?.streamed && typeof body === "string") {
    const s = sseText(body);
    outText = s.text;
    outTools = s.tools;
  } else if (body && typeof body === "object") {
    const m = body.choices?.[0]?.message;
    outText = contentText(m?.content ?? body.choices?.[0]?.text ?? "");
    outTools = (m?.tool_calls ?? []).map((t: any) => `${t.function?.name}(${t.function?.arguments ?? ""})`);
  }
  const timing = call.response?.timing ?? [];
  const tmax = timing.length ? timing[timing.length - 1][1] : 0;

  return (
    <div className="call">
      {header}
      <div className="transcript">
        {messages.map((m, i) => (
          <div key={i} className={`msg ${m.role}`}>
            <div className="role">{m.role}{m.name ? ` · ${m.name}` : ""}</div>
            {m.content != null && m.content !== "" && <div className="content">{contentText(m.content)}</div>}
            {(m.tool_calls ?? []).map((t: any, j: number) => (
              <div key={j} className="toolcall">→ {t.function?.name}({t.function?.arguments})</div>
            ))}
          </div>
        ))}
        <div className="msg assistant answer">
          <div className="role">response{call.response ? ` · ${call.response.status}` : ""}</div>
          {outText && <div className="content">{outText}</div>}
          {outTools.map((t, j) => <div key={j} className="toolcall">→ {t}</div>)}
          {!call.response && <div className="content bad">No response recorded: the run stopped during this call.</div>}
        </div>
      </div>
      {timing.length > 1 && (
        <div className="stream">
          <div className="section-t">token stream · {timing.length} chunks over {fmtVtime(tmax)}</div>
          <svg viewBox="0 0 400 34" preserveAspectRatio="none" className="spark" aria-label="Chunk arrival times">
            {timing.map(([, t], i) => (
              <rect key={i} x={(t / Math.max(tmax, 1)) * 396} y={4} width={3} height={26} rx={1.5} fill="var(--gold)" opacity={0.75} />
            ))}
          </svg>
        </div>
      )}
    </div>
  );
}

// ---------------------------------------------------------------------------------------------
// workspace panel
// ---------------------------------------------------------------------------------------------

function Workspace({ run, index }: { run: LoadedRun; index: number }) {
  const ex = run.view.exchanges[index];
  const next = run.view.exchanges[index + 1];
  const end = run.view.snapshot_list.find((s) => s.when === 2);
  const before = ex?.snapshot;
  const after = next?.snapshot ?? end?.root;
  const [files, setFiles] = useState<FileEntry[]>([]);
  const [changes, setChanges] = useState<FileChange[]>([]);
  const [open, setOpen] = useState<string | null>(null);
  const [content, setContent] = useState<string | null>(null);

  useEffect(() => {
    if (run.handle === undefined || !before) return;
    let live = true;
    kernel().recFiles(run.handle, before).then((f) => live && setFiles(f));
    if (after) kernel().recChanges(run.handle, before, after).then((c) => live && setChanges(c));
    else setChanges([]);
    return () => {
      live = false;
    };
  }, [run, before, after]);

  useEffect(() => {
    if (!open || run.handle === undefined || !before) return setContent(null);
    let live = true;
    kernel().recFile(run.handle, before, open).then((f) => live && setContent(f.missing ? null : f.text ?? "(binary or not stored)"));
    return () => {
      live = false;
    };
  }, [open, run, before]);

  if (run.source !== "khn" || !run.view.snapshots)
    return <div className="empty small">No workspace in this run{run.source === "otel" ? " (traces carry calls only)" : ""}.</div>;
  const changed = new Map(changes.map((c) => [c.path, c]));
  const openChange = open ? changed.get(open) : undefined;
  const added = changes.filter((c) => c.kind === "A" && !files.some((f) => f.path === c.path));

  return (
    <div className="workspace">
      <div className="section-t">files before call #{ex.seq}</div>
      <div className="filelist">
        {files.map((f) => (
          <button key={f.path} className={`file ${open === f.path ? "sel" : ""} ${changed.has(f.path) ? "changed" : ""}`}
            onClick={() => setOpen(open === f.path ? null : f.path)}>
            <span className="mark">{changed.get(f.path)?.kind ?? ""}</span>
            <span className="path">{f.path}</span>
            <span className="size">{f.stored ? fmtBytes(f.size) : "not stored"}</span>
          </button>
        ))}
        {added.map((c) => (
          <button key={c.path} className={`file changed ${open === c.path ? "sel" : ""}`} onClick={() => setOpen(c.path)}>
            <span className="mark">A</span><span className="path">{c.path}</span><span className="size">new</span>
          </button>
        ))}
      </div>
      <div className="section-t">
        {changes.length ? `this call changed ${changes.length} file${changes.length > 1 ? "s" : ""}` : "this call changed no files"}
      </div>
      {open && openChange && openChange.before !== undefined ? (
        <DiffBlock before={openChange.before ?? ""} after={openChange.after ?? ""} />
      ) : open && content !== null ? (
        <pre className="code">{content}</pre>
      ) : null}
    </div>
  );
}

// ---------------------------------------------------------------------------------------------
// compare
// ---------------------------------------------------------------------------------------------

function Compare({ a, b, onPick }: { a: LoadedRun; b: LoadedRun; onPick: (seq: number) => void }) {
  const [d, setD] = useState<RunDiffView | null>(null);
  const [err, setErr] = useState<string | null>(null);
  useEffect(() => {
    if (a.handle === undefined || b.handle === undefined) {
      setErr("Compare works on two .khn recordings.");
      return;
    }
    kernel().recDiff(a.handle, b.handle).then(setD).catch((e) => setErr(String(e)));
  }, [a, b]);
  if (err) return <div className="empty small">{err}</div>;
  if (!d) return <div className="muted">Aligning runs…</div>;
  const first = d.first_divergence !== null ? d.calls[d.first_divergence] : null;
  return (
    <div className="compare">
      <div className="compare-head">
        <b>{a.name}</b> <span className="muted">vs</span> <b>{b.name}</b>
        <span className="spacer" />
        <span className="muted mono">{d.same} identical · tokens {d.a_tokens} → {d.b_tokens}</span>
      </div>
      {d.identical ? (
        <div className="callout good">Identical: same calls, same outputs, same workspace.</div>
      ) : first ? (
        <div className="callout bad">
          <b>First divergence</b> at A#{first.a_seq ?? "—"} / B#{first.b_seq ?? "—"} ({first.label}):{" "}
          {first.kind === "same-request" ? "same request, different output" : first.kind === "changed"
            ? <>the request differs at <code>{first.field}</code></> : first.kind === "only-a" ? "only A made this call" : "only B made this call"}
          {(first.a || first.b) && (
            <div className="ab"><div><span>A</span>{first.a}</div><div><span>B</span>{first.b}</div></div>
          )}
        </div>
      ) : null}
      <div className="diffrows">
        {d.calls.map((c, i) => (
          <button key={i} className={`diffrow ${c.kind}`} onClick={() => c.a_seq !== undefined && onPick(c.a_seq)}>
            <span className="sym">{{ same: "=", "same-request": "~", changed: "≠", "only-a": "−", "only-b": "+" }[c.kind]}</span>
            <span className="mono">A{c.a_seq ?? "—"}</span><span className="mono">B{c.b_seq ?? "—"}</span>
            <span>{c.label}</span>
            {c.field && <span className="muted mono">at {c.field}</span>}
          </button>
        ))}
      </div>
      {d.workspace_end.length > 0 && <div className="muted small">workspace at end: {d.workspace_end.join(", ")}</div>}
    </div>
  );
}

// ---------------------------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------------------------

export function Viewer() {
  const [runs, setRuns] = useState<LoadedRun[]>([]);
  const [sel, setSel] = useState(0);
  const [samples, setSamples] = useState<Sample[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [drag, setDrag] = useState(false);
  const fileInput = useRef<HTMLInputElement>(null);
  const a = runs[0];
  const b = runs[1];

  useEffect(() => {
    fetch(`${BASE}samples/index.json`).then((r) => r.json()).then(setSamples).catch(() => setSamples([]));
    const src = new URLSearchParams(location.hash.split("?")[1] ?? "").get("src");
    if (src) open([{ url: src.startsWith("http") ? src : `${BASE}samples/${src}`, name: src.split("/").pop()! }]);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const open = useCallback(async (items: ({ url: string; name: string } | File)[], compare = false) => {
    setBusy(true);
    setError(null);
    try {
      const loaded: LoadedRun[] = [];
      for (const it of items) {
        if (it instanceof File) loaded.push(await loadBytes(it.name, new Uint8Array(await it.arrayBuffer())));
        else loaded.push(await fetchRun(it.url, it.name));
      }
      setRuns((cur) => (compare && cur[0] ? [cur[0], loaded[0]] : loaded.slice(0, 2)));
      if (!compare) setSel(0);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }, []);

  const ticks: DialTick[] = useMemo(
    () =>
      (a?.view.exchanges ?? []).map((e: ExchangeInfo) => ({
        kind: e.tool ? "tool" : "model",
        replayed: e.from_recording,
        flagged: e.status !== undefined && e.status >= 400,
        label: e.tool ? e.tool_calls : e.model,
      })),
    [a],
  );

  const onDrop = (e: React.DragEvent) => {
    e.preventDefault();
    setDrag(false);
    const files = [...e.dataTransfer.files];
    if (files.length) open(files, e.shiftKey && !!a);
  };

  const pickSeq = (seq: number) => {
    const i = a?.view.exchanges.findIndex((x) => x.seq === seq) ?? -1;
    if (i >= 0) setSel(i);
  };

  const v = a?.view;
  const tokens = v ? v.exchanges.reduce((s, e) => s + e.prompt_tokens + e.completion_tokens, 0) : 0;
  const tools = v ? v.exchanges.filter((e) => e.tool).length : 0;

  return (
    <div className={`viewer ${drag ? "dragging" : ""}`} onDragOver={(e) => { e.preventDefault(); setDrag(true); }}
      onDragLeave={(e) => { if (e.currentTarget === e.target) setDrag(false); }} onDrop={onDrop}>
      <input ref={fileInput} type="file" accept=".khn,.json" multiple hidden
        onChange={(e) => e.target.files?.length && open([...e.target.files])} />
      <PageHelp id="viewer" title="the Viewer" steps={[
        <>Pick a sample on the right (start with <b>Coding agent</b>), or open your own <code>.khn</code> recording or OpenTelemetry trace.</>,
        <>Drag the moon around the <b>Moon Dial</b>, or use the arrow keys, to travel through the run one call at a time.</>,
        <>Read the selected call: the prompt, the answer, any tool calls, and which files that call changed.</>,
        <>Press <b>Compare with a re-run</b> to see where two runs of the same task first went different ways.</>,
      ]} />

      <div className="viewer-bar">
        <button className="btn primary" onClick={() => fileInput.current?.click()} disabled={busy}>Open recording…</button>
        <span className="muted small">.khn from <code>khonsu record</code>, or an OpenTelemetry GenAI trace (OTLP JSON). Nothing leaves your browser.</span>
        <span className="spacer" />
        {samples.map((s) => (
          <button key={s.file} className="btn ghost small" disabled={busy} title={s.blurb}
            onClick={() => open([{ url: `${BASE}samples/${s.file}`, name: s.file }])}>{s.title}</button>
        ))}
      </div>
      {error && <div className="callout bad">{error}</div>}

      {!a ? (
        <div className="dropzone" onClick={() => fileInput.current?.click()}>
          <div className="dz-moon" aria-hidden>☾</div>
          <h2>Drop a recording to travel through it</h2>
          <p className="muted">Every model call, tool call and file change, and the moment it went wrong.</p>
          <div className="dz-samples">
            {samples.map((s) => (
              <button key={s.file} className="sample" onClick={(e) => { e.stopPropagation(); open([{ url: `${BASE}samples/${s.file}`, name: s.file }]); }}>
                <b>{s.title}</b><span>{s.blurb}</span>
              </button>
            ))}
          </div>
        </div>
      ) : (
        <>
          <header className="run-head panel">
            <div className="run-title">
              <h2>{a.name}</h2>
              <code className="cmd">{v!.command}</code>
            </div>
            <div className="run-chips">
              <Chip tone="gold">{v!.mode}</Chip>
              <Chip>{v!.exchanges.length - tools} model · {tools} tool</Chip>
              {tokens > 0 && <Chip>{tokens.toLocaleString()} tokens</Chip>}
              {v!.duration_us !== undefined && <Chip>{fmtVtime(v!.duration_us)}</Chip>}
              {v!.snapshots > 0 && <Chip tone="teal">{v!.snapshots} snapshots · {fmtBytes(v!.object_bytes)}</Chip>}
              {a.source === "khn" && (v!.recovery
                ? <Chip tone="warn">recovered: {v!.recovery}</Chip>
                : v!.chain_ok ? <Chip tone="good">integrity verified</Chip> : <Chip tone="bad">hash chain mismatch</Chip>)}
              {v!.parent && <Chip tone="teal">fork of {v!.parent.slice(0, 10)}</Chip>}
              {v!.complete ? v!.exit_code !== undefined && <Chip tone={v!.exit_code === 0 ? "good" : "bad"}>exit {v!.exit_code}</Chip>
                : <Chip tone="warn">incomplete (crashed)</Chip>}
              <span className="spacer" />
              <button className="btn small" onClick={() => {
                const input = document.createElement("input");
                input.type = "file";
                input.accept = ".khn,.json";
                input.onchange = () => input.files?.length && open([input.files[0]], true);
                input.click();
              }}>Compare with…</button>
              {samples.length > 1 && !b && a.name === samples[0]?.file && (
                <button className="btn small ghost" onClick={() => open([{ url: `${BASE}samples/${samples[1].file}`, name: samples[1].file }], true)}>
                  Compare with a re-run
                </button>
              )}
              {b && <button className="btn small ghost" onClick={() => setRuns([a])}>Close compare</button>}
            </div>
          </header>

          <div className="viewer-grid">
            <aside className="panel dial-panel">
              <MoonDial ticks={ticks} value={Math.min(sel, Math.max(ticks.length - 1, 0))} onChange={setSel} />
              <div className="legend center">
                <span><i className="dot" style={{ background: "var(--gold)" }} /> model</span>
                <span><i className="dot" style={{ background: "var(--faience)" }} /> tool</span>
                <span><i className="dot hollow" /> replayed</span>
              </div>
              <div className="calllist">
                {v!.exchanges.map((e, i) => (
                  <button key={i} className={`callrow ${i === sel ? "sel" : ""}`} onClick={() => setSel(i)}>
                    <span className={`kind ${e.tool ? "tool" : "model"}`} />
                    <span className="mono seq">#{e.seq}</span>
                    <span className="lbl">{e.tool ? e.tool_calls : e.output || e.tool_calls || e.model}</span>
                    {e.workspace_changes.length > 0 && <span className="ws" title={e.workspace_changes.join(", ")}>✎{e.workspace_changes.length}</span>}
                  </button>
                ))}
              </div>
            </aside>
            <section className="panel detail-panel">
              <CallDetail run={a} index={sel} />
            </section>
            <aside className="panel ws-panel">
              <Workspace run={a} index={sel} />
            </aside>
          </div>

          {b && (
            <section className="panel">
              <Compare a={a} b={b} onPick={pickSeq} />
            </section>
          )}
        </>
      )}
      {drag && <div className="drop-overlay">Drop to open{a ? " · hold Shift to compare" : ""}</div>}
    </div>
  );
}
