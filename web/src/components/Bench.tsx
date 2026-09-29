import { useState } from "react";
import { kernel } from "../kernel/client";
import type { BenchForkRow, BenchResult } from "../kernel/types";
import reference from "../lib/reference-bench.json";
import { fmtNs } from "../lib/ui";

const SERIES = [
  { key: "khonsu", label: "Khonsu: persistent Merkle tree", color: "#f5c26b" },
  { key: "v1", label: "v1 design: shared_ptr per page", color: "#a78bfa" },
  { key: "deep", label: "Deep copy (checkpoint = copy)", color: "#ff9e7a" },
] as const;

type Metric = "fork" | "first_write";

function value(row: BenchForkRow, s: (typeof SERIES)[number]["key"], m: Metric): number | undefined {
  return (row as unknown as Record<string, number | undefined>)[`${s}_${m}_ns`];
}

function LogChart({ rows, metric }: { rows: BenchForkRow[]; metric: Metric }) {
  const W = 560, H = 260, L = 64, R = 34, T = 14, B = 36;
  const xs = rows.map((r) => r.mib);
  const vals = rows.flatMap((r) => SERIES.map((s) => value(r, s.key, metric)).filter((v): v is number => !!v && v > 0));
  const lx0 = Math.log10(Math.min(...xs)), lx1 = Math.log10(Math.max(...xs));
  const ly0 = Math.floor(Math.log10(Math.min(...vals))), ly1 = Math.ceil(Math.log10(Math.max(...vals)));
  const X = (v: number) => L + ((Math.log10(v) - lx0) / Math.max(1e-9, lx1 - lx0)) * (W - L - R);
  const Y = (v: number) => T + (1 - (Math.log10(v) - ly0) / Math.max(1e-9, ly1 - ly0)) * (H - T - B);
  return (
    <svg viewBox={`0 0 ${W} ${H}`} style={{ width: "100%", height: "auto" }} role="img"
      aria-label={`${metric} latency versus state size, log-log`}>
      {Array.from({ length: ly1 - ly0 + 1 }, (_, i) => ly0 + i).map((e) => (
        <g key={e}>
          <line x1={L} x2={W - R} y1={Y(10 ** e)} y2={Y(10 ** e)} stroke="#1e2638" />
          <text x={L - 8} y={Y(10 ** e) + 4} fill="#5b6479" fontSize="10.5" textAnchor="end" fontFamily="JetBrains Mono">
            {fmtNs(10 ** e)}
          </text>
        </g>
      ))}
      {xs.map((x) => (
        <text key={x} x={X(x)} y={H - 14} fill="#5b6479" fontSize="10.5" textAnchor="middle" fontFamily="JetBrains Mono">
          {x} MiB
        </text>
      ))}
      <text x={(L + W - R) / 2} y={H - 1} fill="#8a94a7" fontSize="10.5" textAnchor="middle">state size (VFS bytes, 64 files)</text>
      {SERIES.map((s) => {
        const pts = rows.map((r) => [r.mib, value(r, s.key, metric)] as const).filter(([, v]) => !!v && v > 0) as [number, number][];
        if (pts.length === 0) return null;
        return (
          <g key={s.key}>
            <polyline points={pts.map(([x, v]) => `${X(x)},${Y(v)}`).join(" ")} fill="none" stroke={s.color} strokeWidth="2.5"
              strokeLinejoin="round" style={{ filter: `drop-shadow(0 0 6px ${s.color}55)` }} />
            {pts.map(([x, v]) => (
              <g key={x}>
                <circle cx={X(x)} cy={Y(v)} r="4" fill="#07090f" stroke={s.color} strokeWidth="2" />
                <title>{`${s.label}: ${fmtNs(v)} @ ${x} MiB`}</title>
              </g>
            ))}
          </g>
        );
      })}
    </svg>
  );
}

export function Bench() {
  const client = kernel();
  const [result, setResult] = useState<BenchResult>(reference as BenchResult);
  const [live, setLive] = useState(false);
  const [running, setRunning] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  const run = async (quick: boolean) => {
    setRunning(true);
    setErr(null);
    try {
      setResult(await client.bench(quick));
      setLive(true);
    } catch (e) {
      setErr(String(e));
    } finally {
      setRunning(false);
    }
  };

  const last = result.fork[result.fork.length - 1];
  const k = result.kernel;
  return (
    <div className="bench">
      <div className="panel">
        <div className="panel-b" style={{ display: "flex", gap: 16, alignItems: "center", flexWrap: "wrap" }}>
          <div className="prose" style={{ flex: 1, minWidth: 280 }}>
            <h2>Benchmarks</h2>
            Same C++ suite as <code>khonsu_bench</code>, compiled to WebAssembly and run <b>on your machine</b>, in a worker.
            Timing is batched and adaptive because browsers coarsen <code>performance.now()</code>.{" "}
            {live ? <span className="chip good">live result from this browser</span> : <span className="chip">reference: Node 24 · wasm32 · dev laptop</span>}
          </div>
          <div style={{ display: "flex", gap: 8 }}>
            <button className="btn" disabled={running} onClick={() => run(true)}>Quick run (~2 s)</button>
            <button className="btn primary" disabled={running} onClick={() => run(false)}>{running ? "Running…" : "Full run (~6 s, 64 MiB)"}</button>
          </div>
        </div>
        {err && <div className="panel-b"><div className="callout bad">{err}</div></div>}
      </div>

      <div className="panel">
        <div className="panel-b tiles">
          <div className="tile"><div className="k">fork() at {last.mib} MiB</div><div className="v" style={{ color: "var(--accent)" }}>{fmtNs(last.khonsu_fork_ns)}</div><div className="s">root-handle copy: O(1)</div></div>
          <div className="tile"><div className="k">v1 fork at {last.mib} MiB</div><div className="v">{fmtNs(last.v1_fork_ns)}</div><div className="s">{Math.round(last.v1_fork_ns / last.khonsu_fork_ns).toLocaleString()}× slower: O(pages)</div></div>
          {last.deep_fork_ns && <div className="tile"><div className="k">deep copy at {last.mib} MiB</div><div className="v">{fmtNs(last.deep_fork_ns)}</div><div className="s">{Math.round(last.deep_fork_ns / last.khonsu_fork_ns).toLocaleString()}× slower: O(bytes)</div></div>}
          <div className="tile"><div className="k">Kernel::fork (full)</div><div className="v">{fmtNs(k.fork_ns)}</div><div className="s">timeline + CRC'd Fork frame</div></div>
          <div className="tile"><div className="k">verify / replay</div><div className="v">{k.verify_us_per_step.toFixed(1)} µs</div><div className="s">per step, digest-checked</div></div>
          <div className="tile"><div className="k">log append</div><div className="v">{result.log.append_mframes_per_s.toFixed(1)} M/s</div><div className="s">{result.log.append_ns_per_frame.toFixed(0)} ns / 64 B frame</div></div>
          <div className="tile"><div className="k">CRC32C</div><div className="v">{result.log.crc32c_gib_per_s.toFixed(2)} GiB/s</div><div className="s">slicing-by-8 (wasm has no CRC insn)</div></div>
        </div>
      </div>

      <div className="panel half">
        <div className="panel-h"><h2>Fork latency vs state size</h2><span className="hint">log–log</span></div>
        <div className="panel-b"><LogChart rows={result.fork} metric="fork" /><Legend /></div>
      </div>
      <div className="panel half">
        <div className="panel-h"><h2>First write after fork</h2><span className="hint">per page, log–log</span></div>
        <div className="panel-b"><LogChart rows={result.fork} metric="first_write" /><Legend /></div>
      </div>

      <div className="panel">
        <div className="panel-h"><h2>Raw results</h2></div>
        <div className="panel-b" style={{ overflowX: "auto" }}>
          <table className="data">
            <thead><tr><th>state</th><th>Khonsu fork</th><th>v1 fork</th><th>deep fork</th><th>Khonsu 1st write</th><th>v1 1st write</th><th>deep write</th></tr></thead>
            <tbody>
              {result.fork.map((r) => (
                <tr key={r.pages}>
                  <td>{r.mib} MiB · {r.pages} pages</td>
                  <td style={{ color: "var(--accent)" }}>{fmtNs(r.khonsu_fork_ns)}</td>
                  <td>{fmtNs(r.v1_fork_ns)}</td>
                  <td>{r.deep_fork_ns ? fmtNs(r.deep_fork_ns) : "n/a"}</td>
                  <td>{fmtNs(r.khonsu_first_write_ns)}</td>
                  <td>{fmtNs(r.v1_first_write_ns)}</td>
                  <td>{r.deep_first_write_ns ? fmtNs(r.deep_first_write_ns) : "n/a"}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <p className="prose" style={{ fontSize: 12.5, marginTop: 12 }}>
            <b>Reading the trade-off:</b> Khonsu pays on the <i>write</i> path (path-copying ~4 interior nodes plus one 4 KiB page,
            about 1–2 µs) to make fork, seek and snapshot retention free. v1 pays O(pages) atomic ref-count traffic on every fork
            <i> and</i> teardown. Deep copy pays O(bytes). With thousands of speculative branches and every step retained, the
            persistent design wins by orders of magnitude.
          </p>
        </div>
      </div>
    </div>
  );
}

function Legend() {
  return (
    <div className="legend" style={{ marginTop: 6 }}>
      {SERIES.map((s) => (
        <span key={s.key}><i className="dot" style={{ background: s.color }} /> {s.label}</span>
      ))}
    </div>
  );
}
