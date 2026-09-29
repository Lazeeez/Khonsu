import { useEffect, useMemo, useRef, useState } from "react";
import type { SpanInfo, TimelineInfo, World } from "../kernel/types";
import { ACTORS } from "../kernel/types";
import { ACTOR_COLORS, fmtVtime } from "../lib/ui";
import type { Sel } from "./Multiverse";

const ROW = 26;
const TOP = 22;
const LEFT = 92;
const ROWS = [1, 2, 3, 4, 5, 6, 0]; // planner … supervisor, user

/** Spans visible from a timeline: its own plus its ancestors' up to each fork point. */
export function spansFor(world: World, tid: number): SpanInfo[] {
  const out: SpanInfo[] = [];
  let t: TimelineInfo | undefined = world.timelines[tid];
  let limit = Infinity;
  while (t) {
    for (const s of t.spans) if (s.g <= limit) out.push(s);
    limit = t.base;
    t = t.parent >= 0 ? world.timelines[t.parent] : undefined;
  }
  return out;
}

export function Trace({ world, sel }: { world: World; sel: Sel }) {
  const wrap = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const [width, setWidth] = useState(700);
  const [hover, setHover] = useState<{ x: number; y: number; s: SpanInfo } | null>(null);
  const height = TOP + ROWS.length * ROW + 24;

  const spans = useMemo(() => spansFor(world, sel.tid), [world, sel.tid]);
  const tmax = Math.max(1_000_000, ...spans.map((s) => s.t1)) * 1.04;
  const selT = useMemo(() => {
    const t = world.timelines[sel.tid];
    const rec = t?.steps.find((s) => s.g === sel.g);
    if (rec) return rec.vtime;
    return spans.filter((s) => s.g <= sel.g).reduce((m, s) => Math.max(m, s.t0), 0);
  }, [world, sel, spans]);

  useEffect(() => {
    const el = wrap.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setWidth(el.clientWidth));
    ro.observe(el);
    setWidth(el.clientWidth);
    return () => ro.disconnect();
  }, []);

  const X = (t: number) => LEFT + (t / tmax) * (width - LEFT - 12);

  useEffect(() => {
    const c = canvas.current;
    if (!c) return;
    const dpr = window.devicePixelRatio || 1;
    c.width = Math.round(width * dpr);
    c.height = Math.round(height * dpr);
    c.style.height = `${height}px`;
    const ctx = c.getContext("2d")!;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);

    // Time axis.
    ctx.font = "10px JetBrains Mono, monospace";
    const ticks = Math.max(2, Math.min(8, Math.floor((width - LEFT) / 90)));
    for (let i = 0; i <= ticks; i++) {
      const t = (tmax / ticks) * i;
      const x = X(t);
      ctx.strokeStyle = "rgba(30,38,56,.6)";
      ctx.beginPath();
      ctx.moveTo(x, TOP - 4);
      ctx.lineTo(x, height - 18);
      ctx.stroke();
      ctx.fillStyle = "#5b6479";
      ctx.textAlign = "center";
      ctx.fillText(fmtVtime(Math.round(t)), x, height - 6);
    }
    ctx.textAlign = "left";

    // Rows.
    ROWS.forEach((a, i) => {
      const y = TOP + i * ROW;
      ctx.fillStyle = i % 2 ? "rgba(18,24,38,.35)" : "rgba(7,9,15,.2)";
      ctx.fillRect(LEFT, y, width - LEFT, ROW);
      ctx.fillStyle = ACTOR_COLORS[a];
      ctx.beginPath();
      ctx.arc(14, y + ROW / 2, 3.5, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = "#8a94a7";
      ctx.font = "500 11px Inter, sans-serif";
      ctx.fillText(ACTORS[a], 24, y + ROW / 2 + 4);
    });

    // Hatch pattern for cached spans.
    const pc = document.createElement("canvas");
    pc.width = pc.height = 6;
    const p = pc.getContext("2d")!;
    p.strokeStyle = "rgba(127,219,202,.8)";
    p.lineWidth = 1;
    p.beginPath();
    p.moveTo(0, 6);
    p.lineTo(6, 0);
    p.stroke();
    const hatch = ctx.createPattern(pc, "repeat")!;

    for (const s of spans) {
      const row = ROWS.indexOf(s.a);
      if (row < 0) continue;
      const y = TOP + row * ROW;
      const x0 = X(s.t0);
      const x1 = Math.max(x0 + 2, X(s.t1));
      const color = ACTOR_COLORS[s.a];
      const future = s.g > sel.g;
      ctx.globalAlpha = future ? 0.28 : 1;
      if (s.k === "step") {
        ctx.fillStyle = color;
        ctx.fillRect(x0 - 1, y + 5, 2, ROW - 10);
      } else {
        const r = 5;
        ctx.beginPath();
        ctx.roundRect(x0, y + 5, x1 - x0, ROW - 10, r);
        if (s.c) {
          ctx.fillStyle = hatch;
          ctx.fill();
          ctx.strokeStyle = "rgba(127,219,202,.9)";
          ctx.stroke();
        } else {
          const g = ctx.createLinearGradient(0, y, 0, y + ROW);
          g.addColorStop(0, color);
          g.addColorStop(1, shade(color));
          ctx.fillStyle = g;
          ctx.fill();
          if (s.k === "tool") {
            ctx.strokeStyle = "rgba(255,255,255,.55)";
            ctx.setLineDash([3, 3]);
            ctx.stroke();
            ctx.setLineDash([]);
          }
        }
        if (x1 - x0 > 60) {
          ctx.fillStyle = s.c ? "#bff5ea" : "#0b0f18";
          ctx.font = "600 10.5px Inter, sans-serif";
          ctx.save();
          ctx.beginPath();
          ctx.rect(x0, y, x1 - x0 - 4, ROW);
          ctx.clip();
          ctx.fillText(`${s.n}${s.c ? " (cached)" : ""}`, x0 + 6, y + ROW / 2 + 4);
          ctx.restore();
        }
      }
      ctx.globalAlpha = 1;
    }

    // Now-cursor at the selected step's virtual time.
    const cx = X(selT);
    ctx.strokeStyle = "#f5c26b";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(cx, TOP - 6);
    ctx.lineTo(cx, height - 18);
    ctx.stroke();
    ctx.fillStyle = "#f5c26b";
    ctx.beginPath();
    ctx.moveTo(cx - 5, TOP - 10);
    ctx.lineTo(cx + 5, TOP - 10);
    ctx.lineTo(cx, TOP - 4);
    ctx.fill();
  }, [spans, width, height, sel, selT, tmax]);

  const pick = (e: React.MouseEvent) => {
    const r = canvas.current!.getBoundingClientRect();
    const mx = e.clientX - r.left;
    const my = e.clientY - r.top;
    const row = Math.floor((my - TOP) / ROW);
    const a = ROWS[row];
    if (a === undefined) return null;
    const hit = spans
      .filter((s) => s.a === a && s.k !== "step" && X(s.t0) - 2 <= mx && mx <= Math.max(X(s.t0) + 4, X(s.t1)) + 2)
      .pop();
    return hit ? { x: mx, y: my, s: hit } : null;
  };

  return (
    <div className="canvas-wrap" ref={wrap}>
      <canvas ref={canvas} onMouseMove={(e) => setHover(pick(e))} onMouseLeave={() => setHover(null)}
        role="img" aria-label="Causal trace: spans per agent over virtual time" />
      {hover && (
        <div className="tooltip" style={{ left: Math.min(hover.x + 12, width - 260), top: hover.y + 12 }}>
          <b>{hover.s.n}</b> {hover.s.c && <span className="chip teal">cached</span>}
          <div className="t">
            step {hover.s.g} · {fmtVtime(hover.s.t0)} → {fmtVtime(hover.s.t1)} ({fmtVtime(hover.s.t1 - hover.s.t0)})
          </div>
        </div>
      )}
    </div>
  );
}

function shade(hex: string): string {
  const n = parseInt(hex.slice(1), 16);
  const r = Math.round(((n >> 16) & 255) * 0.72);
  const g = Math.round(((n >> 8) & 255) * 0.72);
  const b = Math.round((n & 255) * 0.72);
  return `rgb(${r},${g},${b})`;
}
