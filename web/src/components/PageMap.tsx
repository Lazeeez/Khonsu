import { useEffect, useState } from "react";
import type { KernelClient } from "../kernel/client";
import type { PageInfo, StateEntry, World } from "../kernel/types";
import { fmtBytes } from "../lib/ui";
import type { Sel } from "./Multiverse";

type Baseline = "fork" | "prev" | "genesis";

export function PageMap({ client, world, sel, files, version }: {
  client: KernelClient;
  world: World;
  sel: Sel;
  files: StateEntry[];
  version: number;
}) {
  const [path, setPath] = useState("/repo/src/generated/client.ts");
  const [baseline, setBaseline] = useState<Baseline>("fork");
  const [data, setData] = useState<{ size: number; pages: PageInfo[] } | null>(null);

  const t = world.timelines[sel.tid];
  const isFork = t && t.parent >= 0;
  const mode: Baseline = baseline === "fork" && !isFork ? "prev" : baseline;
  const base: [number, number] =
    mode === "fork" ? [t.parent, t.base] : mode === "prev" ? [sel.tid, Math.max(0, sel.g - 1)] : [sel.tid, 0];

  useEffect(() => {
    let live = true;
    client
      .pages(sel.tid, sel.g, path, base[0], base[1])
      .then((d) => live && setData(d))
      .catch(() => live && setData(null));
    return () => {
      live = false;
    };
  }, [client, sel.tid, sel.g, path, base[0], base[1], version]);

  const counts = { shared: 0, private: 0, new: 0 };
  data?.pages.forEach((p) => counts[p.s]++);
  const pageFiles = files.filter((f) => f.type === "file");

  return (
    <div className="panel">
      <div className="panel-h">
        <h2>Copy-on-write pages</h2>
        <span className="spacer" />
        <select value={mode} onChange={(e) => setBaseline(e.target.value as Baseline)} aria-label="Baseline">
          {isFork && <option value="fork">vs fork point</option>}
          <option value="prev">vs previous step</option>
          <option value="genesis">vs genesis</option>
        </select>
      </div>
      <div className="panel-b" style={{ display: "grid", gap: 10 }}>
        <select value={path} onChange={(e) => setPath(e.target.value)} aria-label="File" style={{ width: "100%" }}>
          {pageFiles.map((f) => (
            <option key={f.key} value={f.key}>
              {f.key} · {f.pages} pg
            </option>
          ))}
        </select>
        {data && data.pages.length > 0 ? (
          <>
            <div className="pagegrid">
              {data.pages.map((p) => (
                <div key={p.i} className={`page ${p.s}`} title={`page ${p.i} · ${p.s} · node #${p.ref & 0xfffffff} · ${p.d}`}>
                  {p.i}
                </div>
              ))}
            </div>
            <div className="legend">
              <span><i className="dot" style={{ background: "#1d2740", border: "1px solid #2a3450" }} /> shared {counts.shared}</span>
              <span><i className="dot" style={{ background: "#f5c26b" }} /> private {counts.private}</span>
              <span><i className="dot" style={{ background: "#7fdbca" }} /> new {counts.new}</span>
              <span className="mono">{fmtBytes(data.size)} · 4 KiB pages</span>
            </div>
            <div style={{ color: "var(--text-3)", fontSize: 11.5 }}>
              {counts.private + counts.new === 0
                ? "Every page is physically shared with the baseline: zero bytes copied."
                : `Only ${counts.private + counts.new} of ${data.pages.length} pages were copied (${fmtBytes(
                    (counts.private + counts.new) * 4096,
                  )}); the rest are the same physical nodes.`}
            </div>
          </>
        ) : (
          <div style={{ color: "var(--text-3)" }}>File does not exist at this step.</div>
        )}
      </div>
    </div>
  );
}
